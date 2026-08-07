// B+ Tree based key-value file storage
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <unordered_map>

static const int M = 52;        // max keys per node
static const int MINK = M / 2;  // 26
static const int PAGE_SZ = 4096;
static const char *DB_FILE = "bpt_storage.dat";

struct Key {
    char s[65];
    int v;
};

static inline int cmpKey(const Key &a, const Key &b) {
    int c = strcmp(a.s, b.s);
    if (c) return c < 0 ? -1 : 1;
    if (a.v < b.v) return -1;
    if (a.v > b.v) return 1;
    return 0;
}

struct Node {
    int isLeaf;
    int size;
    int next;  // leaf: next leaf id ; free page: next free
    int pad;
    Key keys[M + 1];
    int child[M + 2];
};

static_assert(sizeof(Node) <= PAGE_SZ, "node too big");

// ------------------- cache -------------------
static const int NFRAMES = 5000;
static Node *pool = nullptr;
static int framePid[NFRAMES];
static bool frameDirty[NFRAMES];
static int lruPrev[NFRAMES], lruNext[NFRAMES];
static int lruHead = -1, lruTail = -1;
static int usedFrames = 0;
static std::unordered_map<int, int> table;

static FILE *fp = nullptr;
static int g_root = 0, g_pageCount = 1, g_freeHead = 0;

static void lruRemove(int i) {
    if (lruPrev[i] != -1) lruNext[lruPrev[i]] = lruNext[i];
    else lruHead = lruNext[i];
    if (lruNext[i] != -1) lruPrev[lruNext[i]] = lruPrev[i];
    else lruTail = lruPrev[i];
    lruPrev[i] = lruNext[i] = -1;
}
static void lruPushFront(int i) {
    lruPrev[i] = -1;
    lruNext[i] = lruHead;
    if (lruHead != -1) lruPrev[lruHead] = i;
    lruHead = i;
    if (lruTail == -1) lruTail = i;
}

static void diskRead(int pid, Node *dst) {
    if (fseek(fp, (long long)pid * PAGE_SZ, SEEK_SET) != 0) {
        memset(dst, 0, sizeof(Node));
        return;
    }
    size_t r = fread(dst, sizeof(Node), 1, fp);
    if (r != 1) {
        memset(dst, 0, sizeof(Node));
        clearerr(fp);
    }
}
static void diskWrite(int pid, const Node *src) {
    fseek(fp, (long long)pid * PAGE_SZ, SEEK_SET);
    fwrite(src, sizeof(Node), 1, fp);
}

static Node *fetch(int pid, bool dirty = false) {
    auto it = table.find(pid);
    if (it != table.end()) {
        int i = it->second;
        lruRemove(i);
        lruPushFront(i);
        if (dirty) frameDirty[i] = true;
        return &pool[i];
    }
    int i;
    if (usedFrames < NFRAMES) {
        i = usedFrames++;
        lruPrev[i] = lruNext[i] = -1;
    } else {
        i = lruTail;
        if (frameDirty[i]) diskWrite(framePid[i], &pool[i]);
        table.erase(framePid[i]);
        lruRemove(i);
    }
    diskRead(pid, &pool[i]);
    framePid[i] = pid;
    frameDirty[i] = dirty;
    table[pid] = i;
    lruPushFront(i);
    return &pool[i];
}

static void copyNode(int pid, Node *dst) {
    Node *p = fetch(pid);
    memcpy(dst, p, sizeof(Node));
}
static void putNode(int pid, const Node *src) {
    Node *p = fetch(pid, true);
    memcpy(p, src, sizeof(Node));
}

static int allocPage() {
    if (g_freeHead != 0) {
        int pid = g_freeHead;
        Node *n = fetch(pid, true);
        g_freeHead = n->next;
        memset(n, 0, sizeof(Node));
        return pid;
    }
    int pid = g_pageCount++;
    Node *n = fetch(pid, true);
    memset(n, 0, sizeof(Node));
    return pid;
}
static void freePage(int pid) {
    Node *n = fetch(pid, true);
    memset(n, 0, sizeof(Node));
    n->next = g_freeHead;
    g_freeHead = pid;
}

static void flushAll() {
    for (int i = 0; i < usedFrames; i++)
        if (frameDirty[i]) {
            diskWrite(framePid[i], &pool[i]);
            frameDirty[i] = false;
        }
    int hdr[4] = {g_root, g_pageCount, g_freeHead, 20250807};
    fseek(fp, 0, SEEK_SET);
    fwrite(hdr, sizeof(int), 4, fp);
    fflush(fp);
}

// ------------------- search helpers -------------------
static inline int lowerBound(const Node *n, const Key &k) {
    int lo = 0, hi = n->size;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (cmpKey(n->keys[mid], k) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
// first index with keys[i] > k
static inline int upperBound(const Node *n, const Key &k) {
    int lo = 0, hi = n->size;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (cmpKey(n->keys[mid], k) <= 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// ------------------- insert -------------------
static bool insertRec(int pid, const Key &k, Key &upKey, int &newPid) {
    Node *n = fetch(pid);
    if (n->isLeaf) {
        int pos = lowerBound(n, k);
        if (pos < n->size && cmpKey(n->keys[pos], k) == 0) return false;
        n = fetch(pid, true);
        memmove(&n->keys[pos + 1], &n->keys[pos], (size_t)(n->size - pos) * sizeof(Key));
        n->keys[pos] = k;
        n->size++;
        if (n->size <= M) return false;
        Node left;
        memcpy(&left, n, sizeof(Node));
        int total = left.size;
        int mid = total / 2;
        Node right;
        memset(&right, 0, sizeof(Node));
        right.isLeaf = 1;
        right.size = total - mid;
        memcpy(right.keys, left.keys + mid, (size_t)right.size * sizeof(Key));
        left.size = mid;
        int rp = allocPage();
        right.next = left.next;
        left.next = rp;
        putNode(rp, &right);
        putNode(pid, &left);
        upKey = right.keys[0];
        newPid = rp;
        return true;
    }
    int ci = upperBound(n, k);
    int c = n->child[ci];
    Key up;
    int np;
    if (!insertRec(c, k, up, np)) return false;
    Node cur;
    copyNode(pid, &cur);
    memmove(&cur.keys[ci + 1], &cur.keys[ci], (size_t)(cur.size - ci) * sizeof(Key));
    memmove(&cur.child[ci + 2], &cur.child[ci + 1], (size_t)(cur.size - ci) * sizeof(int));
    cur.keys[ci] = up;
    cur.child[ci + 1] = np;
    cur.size++;
    if (cur.size <= M) {
        putNode(pid, &cur);
        return false;
    }
    int mid = cur.size / 2;
    upKey = cur.keys[mid];
    Node right;
    memset(&right, 0, sizeof(Node));
    right.isLeaf = 0;
    right.size = cur.size - mid - 1;
    memcpy(right.keys, cur.keys + mid + 1, (size_t)right.size * sizeof(Key));
    memcpy(right.child, cur.child + mid + 1, (size_t)(right.size + 1) * sizeof(int));
    cur.size = mid;
    int rp = allocPage();
    putNode(rp, &right);
    putNode(pid, &cur);
    newPid = rp;
    return true;
}

static void insertKey(const Key &k) {
    Key up;
    int np;
    if (insertRec(g_root, k, up, np)) {
        Node nr;
        memset(&nr, 0, sizeof(Node));
        nr.isLeaf = 0;
        nr.size = 1;
        nr.keys[0] = up;
        int oldRoot = g_root;
        int rp = allocPage();
        nr.child[0] = oldRoot;
        nr.child[1] = np;
        putNode(rp, &nr);
        g_root = rp;
    }
}

// ------------------- erase -------------------
static bool fixChild(int pid, int ci) {
    Node par;
    copyNode(pid, &par);
    int cpid = par.child[ci];
    Node ch;
    copyNode(cpid, &ch);
    if (ci > 0) {
        int lpid = par.child[ci - 1];
        Node lf;
        copyNode(lpid, &lf);
        if (lf.size > MINK) {
            if (ch.isLeaf) {
                memmove(&ch.keys[1], &ch.keys[0], (size_t)ch.size * sizeof(Key));
                ch.keys[0] = lf.keys[lf.size - 1];
                ch.size++;
                lf.size--;
                par.keys[ci - 1] = ch.keys[0];
            } else {
                memmove(&ch.keys[1], &ch.keys[0], (size_t)ch.size * sizeof(Key));
                memmove(&ch.child[1], &ch.child[0], (size_t)(ch.size + 1) * sizeof(int));
                ch.keys[0] = par.keys[ci - 1];
                ch.child[0] = lf.child[lf.size];
                ch.size++;
                par.keys[ci - 1] = lf.keys[lf.size - 1];
                lf.size--;
            }
            putNode(lpid, &lf);
            putNode(cpid, &ch);
            putNode(pid, &par);
            return false;
        }
    }
    if (ci < par.size) {
        int rpid = par.child[ci + 1];
        Node rt;
        copyNode(rpid, &rt);
        if (rt.size > MINK) {
            if (ch.isLeaf) {
                ch.keys[ch.size++] = rt.keys[0];
                memmove(&rt.keys[0], &rt.keys[1], (size_t)(rt.size - 1) * sizeof(Key));
                rt.size--;
                par.keys[ci] = rt.keys[0];
            } else {
                ch.keys[ch.size] = par.keys[ci];
                ch.child[ch.size + 1] = rt.child[0];
                ch.size++;
                par.keys[ci] = rt.keys[0];
                memmove(&rt.keys[0], &rt.keys[1], (size_t)(rt.size - 1) * sizeof(Key));
                memmove(&rt.child[0], &rt.child[1], (size_t)rt.size * sizeof(int));
                rt.size--;
            }
            putNode(rpid, &rt);
            putNode(cpid, &ch);
            putNode(pid, &par);
            return false;
        }
    }
    // merge
    if (ci > 0) {
        int lpid = par.child[ci - 1];
        Node lf;
        copyNode(lpid, &lf);
        if (ch.isLeaf) {
            memcpy(&lf.keys[lf.size], ch.keys, (size_t)ch.size * sizeof(Key));
            lf.size += ch.size;
            lf.next = ch.next;
        } else {
            lf.keys[lf.size] = par.keys[ci - 1];
            memcpy(&lf.keys[lf.size + 1], ch.keys, (size_t)ch.size * sizeof(Key));
            memcpy(&lf.child[lf.size + 1], ch.child, (size_t)(ch.size + 1) * sizeof(int));
            lf.size += ch.size + 1;
        }
        putNode(lpid, &lf);
        freePage(cpid);
        memmove(&par.keys[ci - 1], &par.keys[ci], (size_t)(par.size - ci) * sizeof(Key));
        memmove(&par.child[ci], &par.child[ci + 1], (size_t)(par.size - ci) * sizeof(int));
        par.size--;
        putNode(pid, &par);
        return par.size < MINK;
    } else {
        int rpid = par.child[1];
        Node rt;
        copyNode(rpid, &rt);
        if (ch.isLeaf) {
            memcpy(&ch.keys[ch.size], rt.keys, (size_t)rt.size * sizeof(Key));
            ch.size += rt.size;
            ch.next = rt.next;
        } else {
            ch.keys[ch.size] = par.keys[0];
            memcpy(&ch.keys[ch.size + 1], rt.keys, (size_t)rt.size * sizeof(Key));
            memcpy(&ch.child[ch.size + 1], rt.child, (size_t)(rt.size + 1) * sizeof(int));
            ch.size += rt.size + 1;
        }
        putNode(cpid, &ch);
        freePage(rpid);
        memmove(&par.keys[0], &par.keys[1], (size_t)(par.size - 1) * sizeof(Key));
        memmove(&par.child[1], &par.child[2], (size_t)(par.size - 1) * sizeof(int));
        par.size--;
        putNode(pid, &par);
        return par.size < MINK;
    }
}

static bool eraseRec(int pid, const Key &k) {
    Node *n = fetch(pid);
    if (n->isLeaf) {
        int pos = lowerBound(n, k);
        if (pos < n->size && cmpKey(n->keys[pos], k) == 0) {
            n = fetch(pid, true);
            memmove(&n->keys[pos], &n->keys[pos + 1], (size_t)(n->size - pos - 1) * sizeof(Key));
            n->size--;
            return n->size < MINK;
        }
        return false;
    }
    int ci = upperBound(n, k);
    int c = n->child[ci];
    if (!eraseRec(c, k)) return false;
    return fixChild(pid, ci);
}

static void eraseKey(const Key &k) {
    if (eraseRec(g_root, k)) {
        Node *r = fetch(g_root);
        if (!r->isLeaf && r->size == 0) {
            int c = r->child[0];
            int old = g_root;
            g_root = c;
            freePage(old);
        }
    }
}

// ------------------- io -------------------
static char inbuf[1 << 16];
static size_t inpos = 0, inlen = 0;
static inline int gc() {
    if (inpos == inlen) {
        inlen = fread(inbuf, 1, sizeof(inbuf), stdin);
        inpos = 0;
        if (inlen == 0) return -1;
    }
    return (unsigned char)inbuf[inpos++];
}
static char obuf[1 << 16];
static size_t opos = 0;
static inline void oflush() {
    if (opos) fwrite(obuf, 1, opos, stdout);
    opos = 0;
}
static inline void oc(char c) {
    if (opos == sizeof(obuf)) oflush();
    obuf[opos++] = c;
}
static void oint(int x) {
    char tmp[12];
    int t = 0;
    unsigned int ux;
    if (x < 0) {
        oc('-');
        ux = (unsigned int)(-(long long)x);
    } else ux = (unsigned int)x;
    if (ux == 0) tmp[t++] = '0';
    while (ux) {
        tmp[t++] = (char)('0' + ux % 10);
        ux /= 10;
    }
    while (t) oc(tmp[--t]);
}
static bool readToken(char *dst, int cap) {
    int c = gc();
    while (c == ' ' || c == '\n' || c == '\r' || c == '\t') c = gc();
    if (c == -1) return false;
    int len = 0;
    while (c != -1 && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
        if (len < cap - 1) dst[len++] = (char)c;
        c = gc();
    }
    dst[len] = 0;
    return true;
}
static bool readInt(int &out) {
    char buf[32];
    if (!readToken(buf, 32)) return false;
    out = (int)strtol(buf, nullptr, 10);
    return true;
}

static void findAll(const char *idx) {
    Key k;
    memset(&k, 0, sizeof(Key));
    strncpy(k.s, idx, 64);
    k.v = INT_MIN;
    int pid = g_root;
    for (;;) {
        Node *n = fetch(pid);
        if (n->isLeaf) break;
        pid = n->child[upperBound(n, k)];
    }
    Node *n = fetch(pid);
    int pos = lowerBound(n, k);
    bool any = false;
    bool stop = false;
    while (pid != 0 && !stop) {
        n = fetch(pid);
        while (pos < n->size) {
            if (strcmp(n->keys[pos].s, k.s) != 0) {
                stop = true;
                break;
            }
            if (any) oc(' ');
            oint(n->keys[pos].v);
            any = true;
            pos++;
        }
        if (stop) break;
        pid = n->next;
        pos = 0;
    }
    if (!any) {
        oc('n');
        oc('u');
        oc('l');
        oc('l');
    }
    oc('\n');
}

int main() {
    pool = (Node *)malloc(sizeof(Node) * NFRAMES);
    if (!pool) return 1;
    fp = fopen(DB_FILE, "r+b");
    bool fresh = false;
    if (!fp) {
        fp = fopen(DB_FILE, "w+b");
        if (!fp) return 1;
        fresh = true;
    } else {
        int hdr[4] = {0, 0, 0, 0};
        if (fread(hdr, sizeof(int), 4, fp) != 4 || hdr[3] != 20250807) fresh = true;
        else {
            g_root = hdr[0];
            g_pageCount = hdr[1];
            g_freeHead = hdr[2];
            if (g_root <= 0 || g_pageCount <= 1) fresh = true;
        }
    }
    if (fresh) {
        g_pageCount = 1;
        g_freeHead = 0;
        g_root = allocPage();
        Node *r = fetch(g_root, true);
        memset(r, 0, sizeof(Node));
        r->isLeaf = 1;
        r->size = 0;
        r->next = 0;
    }

    int n;
    if (!readInt(n)) {
        flushAll();
        return 0;
    }
    char cmd[16], idx[80];
    for (int i = 0; i < n; i++) {
        if (!readToken(cmd, 16)) break;
        if (cmd[0] == 'i') {
            if (!readToken(idx, 80)) break;
            int v;
            if (!readInt(v)) break;
            Key k;
            memset(&k, 0, sizeof(Key));
            strncpy(k.s, idx, 64);
            k.v = v;
            insertKey(k);
        } else if (cmd[0] == 'd') {
            if (!readToken(idx, 80)) break;
            int v;
            if (!readInt(v)) break;
            Key k;
            memset(&k, 0, sizeof(Key));
            strncpy(k.s, idx, 64);
            k.v = v;
            eraseKey(k);
        } else if (cmd[0] == 'f') {
            if (!readToken(idx, 80)) break;
            findAll(idx);
        }
    }
    oflush();
    flushAll();
    fclose(fp);
    return 0;
}
