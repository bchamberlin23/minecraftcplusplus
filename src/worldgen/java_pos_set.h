#pragma once
// java.util.HashSet<BlockPos> with the JDK's exact iteration order: power-of-two table, (h ^ h >>> 16)
// bucket spreading, insertion-ordered bins, order-preserving resize splits, and red-black "tree bins"
// (treeify at 9 entries once the table has 64 buckets, untreeify on split). Features that iterate the
// game's HashSets (TreeFeature's logs/leaves, its leaf distance queues, fallen logs) depend on this order.
//
// Only equal-hash keys inside one tree bin are ordered by System.identityHashCode in the JDK, which is not
// reproducible by anyone; insertion order stands in for it.
#include "worldgen/feature_api.h"
#include "worldgen/rng.h"

#include <vector>

namespace wg {

class JavaPosSet {
public:
    static i32 hashOf(const BlockPos& p) {  // Vec3i.hashCode, then HashMap.hash
        const i32 h = jadd32(jmul32(jadd32(p.y, jmul32(p.z, 31)), 31), p.x);
        return h ^ i32(u32(h) >> 16);
    }

    bool add(const BlockPos& p) { return put(hashOf(p), p); }
    bool contains(const BlockPos& p) const { return find(hashOf(p), p) >= 0; }
    bool empty() const { return size_ == 0; }
    int size() const { return size_; }

    template <class F>
    void forEach(F&& f) const {
        for (i32 head : table_)
            for (i32 e = head; e >= 0; e = nodes_[e].next) f(nodes_[e].pos);
    }
    std::vector<BlockPos> toList() const {
        std::vector<BlockPos> out;
        out.reserve(size_t(size_));
        forEach([&](const BlockPos& p) { out.push_back(p); });
        return out;
    }
    // iterator().next() followed by iterator.remove(). The set must not be empty.
    BlockPos removeFirst() {
        size_t index = 0;
        while (table_[index] < 0) ++index;
        const i32 node = table_[index];
        const BlockPos p = nodes_[node].pos;
        if (nodes_[node].tree)
            removeTreeNode(node, false);
        else
            table_[index] = nodes_[node].next;
        --size_;
        return p;
    }

private:
    static constexpr int kTreeifyThreshold = 8, kUntreeifyThreshold = 6, kMinTreeifyCapacity = 64;
    struct Node {
        BlockPos pos;
        i32 hash;
        u32 seq;  // stands in for the key's identity hash
        i32 next = -1, prev = -1, parent = -1, left = -1, right = -1;
        bool red = false, tree = false;
    };
    static i32 jadd32(i32 a, i32 b) { return i32(u32(a) + u32(b)); }

    Node& n(i32 i) { return nodes_[size_t(i)]; }
    i32 newNode(i32 hash, const BlockPos& p) {
        Node node;
        node.pos = p;
        node.hash = hash;
        node.seq = seq_++;
        nodes_.push_back(node);
        return i32(nodes_.size() - 1);
    }
    i32 bucket(i32 hash) const { return hash & i32(table_.size() - 1); }

    i32 find(i32 hash, const BlockPos& p) const {
        if (table_.empty()) return -1;
        for (i32 e = table_[size_t(bucket(hash))]; e >= 0; e = nodes_[e].next)
            if (nodes_[e].hash == hash && nodes_[e].pos == p) return e;
        return -1;
    }

    bool put(i32 hash, const BlockPos& p) {
        if (table_.empty()) resize();
        const i32 i = bucket(hash);
        i32 first = table_[size_t(i)];
        if (first < 0) {
            table_[size_t(i)] = newNode(hash, p);
        } else {
            if (find(hash, p) >= 0) return false;
            if (n(first).tree) {
                putTreeVal(first, hash, p);
            } else {
                i32 last = first;
                int binCount = 0;
                while (n(last).next >= 0) last = n(last).next, ++binCount;
                const i32 x = newNode(hash, p);
                n(last).next = x;
                if (binCount >= kTreeifyThreshold - 1) treeifyBin(hash);
            }
        }
        if (++size_ > threshold_) resize();
        return true;
    }

    void resize() {
        const int oldCap = int(table_.size());
        int newCap, newThr;
        if (oldCap > 0) {
            newCap = oldCap << 1;
            newThr = threshold_ << 1;
        } else {
            newCap = 16;
            newThr = 12;
        }
        threshold_ = newThr;
        std::vector<i32> old = std::move(table_);
        table_.assign(size_t(newCap), -1);
        for (int j = 0; j < oldCap; ++j) {
            const i32 e = old[size_t(j)];
            if (e < 0) continue;
            if (n(e).next < 0) {
                table_[size_t(n(e).hash & (newCap - 1))] = e;
            } else if (n(e).tree) {
                split(e, j, oldCap);
            } else {
                i32 loHead = -1, loTail = -1, hiHead = -1, hiTail = -1;
                for (i32 x = e, next; x >= 0; x = next) {
                    next = n(x).next;
                    if ((n(x).hash & oldCap) == 0) {
                        (loTail < 0 ? loHead : n(loTail).next) = x;
                        loTail = x;
                    } else {
                        (hiTail < 0 ? hiHead : n(hiTail).next) = x;
                        hiTail = x;
                    }
                }
                if (loTail >= 0) n(loTail).next = -1, table_[size_t(j)] = loHead;
                if (hiTail >= 0) n(hiTail).next = -1, table_[size_t(j + oldCap)] = hiHead;
            }
        }
    }

    // ---- tree bins (HashMap.TreeNode) ----
    int tieBreakOrder(i32 a, i32 b) const { return nodes_[a].seq <= nodes_[b].seq ? -1 : 1; }
    i32 rootOf(i32 x) {
        while (n(x).parent >= 0) x = n(x).parent;
        return x;
    }

    void treeifyBin(i32 hash) {
        const int cap = int(table_.size());
        if (cap < kMinTreeifyCapacity) {
            resize();
            return;
        }
        const i32 index = bucket(hash);
        i32 hd = table_[size_t(index)], tl = -1;
        for (i32 e = hd; e >= 0; e = n(e).next) {  // replacementTreeNode keeps the order
            n(e).tree = true;
            n(e).prev = tl;
            n(e).parent = n(e).left = n(e).right = -1;
            tl = e;
        }
        if (hd >= 0) treeify(hd);
    }

    void treeify(i32 head) {
        i32 root = -1;
        for (i32 x = head, next; x >= 0; x = next) {
            next = n(x).next;
            n(x).left = n(x).right = -1;
            if (root < 0) {
                n(x).parent = -1;
                n(x).red = false;
                root = x;
            } else {
                const i32 h = n(x).hash;
                for (i32 p = root;;) {
                    const i32 ph = n(p).hash;
                    const int dir = ph > h ? -1 : ph < h ? 1 : tieBreakOrder(x, p);
                    const i32 xp = p;
                    p = dir <= 0 ? n(p).left : n(p).right;
                    if (p < 0) {
                        n(x).parent = xp;
                        (dir <= 0 ? n(xp).left : n(xp).right) = x;
                        root = balanceInsertion(root, x);
                        break;
                    }
                }
            }
        }
        moveRootToFront(root);
    }

    void untreeify(i32 head, i32 index) {  // replacementNode: plain nodes, same order
        for (i32 q = head; q >= 0; q = n(q).next) n(q).tree = false;
        table_[size_t(index)] = head;
    }

    void moveRootToFront(i32 root) {
        if (root < 0 || table_.empty()) return;
        const i32 index = bucket(n(root).hash);
        const i32 first = table_[size_t(index)];
        if (root != first) {
            table_[size_t(index)] = root;
            const i32 rp = n(root).prev, rn = n(root).next;
            if (rn >= 0) n(rn).prev = rp;
            if (rp >= 0) n(rp).next = rn;
            if (first >= 0) n(first).prev = root;
            n(root).next = first;
            n(root).prev = -1;
        }
    }

    void putTreeVal(i32 first, i32 h, const BlockPos& p) {  // the key is known to be absent
        const i32 root = n(first).parent >= 0 ? rootOf(first) : first;
        const i32 x = newNode(h, p);
        n(x).tree = true;
        for (i32 q = root;;) {
            const i32 ph = n(q).hash;
            const int dir = ph > h ? -1 : ph < h ? 1 : tieBreakOrder(x, q);
            const i32 xp = q;
            q = dir <= 0 ? n(q).left : n(q).right;
            if (q < 0) {
                const i32 xpn = n(xp).next;
                n(x).next = xpn;
                (dir <= 0 ? n(xp).left : n(xp).right) = x;
                n(xp).next = x;
                n(x).parent = n(x).prev = xp;
                if (xpn >= 0) n(xpn).prev = x;
                moveRootToFront(balanceInsertion(root, x));
                return;
            }
        }
    }

    void removeTreeNode(i32 self, bool movable) {
        const i32 index = bucket(n(self).hash);
        i32 first = table_[size_t(index)], root = first;
        const i32 succ = n(self).next, pred = n(self).prev;
        if (pred < 0)
            table_[size_t(index)] = first = succ;
        else
            n(pred).next = succ;
        if (succ >= 0) n(succ).prev = pred;
        if (first < 0) return;
        if (n(root).parent >= 0) root = rootOf(root);
        i32 rl;
        if (root < 0 || (movable && (n(root).right < 0 || (rl = n(root).left) < 0 || n(rl).left < 0))) {
            untreeify(first, index);
            return;
        }
        const i32 p = self, pl = n(self).left, pr = n(self).right;
        i32 replacement;
        if (pl >= 0 && pr >= 0) {
            i32 s = pr, sl;
            while ((sl = n(s).left) >= 0) s = sl;
            std::swap(n(s).red, n(p).red);
            const i32 sr = n(s).right, pp = n(p).parent;
            if (s == pr) {
                n(p).parent = s;
                n(s).right = p;
            } else {
                const i32 sp = n(s).parent;
                if ((n(p).parent = sp) >= 0) (s == n(sp).left ? n(sp).left : n(sp).right) = p;
                if ((n(s).right = pr) >= 0) n(pr).parent = s;
            }
            n(p).left = -1;
            if ((n(p).right = sr) >= 0) n(sr).parent = p;
            if ((n(s).left = pl) >= 0) n(pl).parent = s;
            if ((n(s).parent = pp) < 0)
                root = s;
            else if (p == n(pp).left)
                n(pp).left = s;
            else
                n(pp).right = s;
            replacement = sr >= 0 ? sr : p;
        } else if (pl >= 0) {
            replacement = pl;
        } else if (pr >= 0) {
            replacement = pr;
        } else {
            replacement = p;
        }
        if (replacement != p) {
            const i32 pp = n(replacement).parent = n(p).parent;
            if (pp < 0)
                n(root = replacement).red = false;
            else if (p == n(pp).left)
                n(pp).left = replacement;
            else
                n(pp).right = replacement;
            n(p).left = n(p).right = n(p).parent = -1;
        }
        const i32 r = n(p).red ? root : balanceDeletion(root, replacement);
        if (replacement == p) {
            const i32 pp = n(p).parent;
            n(p).parent = -1;
            if (pp >= 0) {
                if (p == n(pp).left)
                    n(pp).left = -1;
                else if (p == n(pp).right)
                    n(pp).right = -1;
            }
        }
        if (movable) moveRootToFront(r);
    }

    void split(i32 b, int index, int bit) {
        i32 loHead = -1, loTail = -1, hiHead = -1, hiTail = -1;
        int lc = 0, hc = 0;
        for (i32 e = b, next; e >= 0; e = next) {
            next = n(e).next;
            n(e).next = -1;
            if ((n(e).hash & bit) == 0) {
                if ((n(e).prev = loTail) < 0)
                    loHead = e;
                else
                    n(loTail).next = e;
                loTail = e;
                ++lc;
            } else {
                if ((n(e).prev = hiTail) < 0)
                    hiHead = e;
                else
                    n(hiTail).next = e;
                hiTail = e;
                ++hc;
            }
        }
        if (loHead >= 0) {
            if (lc <= kUntreeifyThreshold) {
                untreeify(loHead, index);
            } else {
                table_[size_t(index)] = loHead;
                if (hiHead >= 0) treeify(loHead);
            }
        }
        if (hiHead >= 0) {
            if (hc <= kUntreeifyThreshold) {
                untreeify(hiHead, index + bit);
            } else {
                table_[size_t(index + bit)] = hiHead;
                if (loHead >= 0) treeify(hiHead);
            }
        }
    }

    i32 rotateLeft(i32 root, i32 p) {
        i32 r, pp, rl;
        if (p >= 0 && (r = n(p).right) >= 0) {
            if ((rl = n(p).right = n(r).left) >= 0) n(rl).parent = p;
            if ((pp = n(r).parent = n(p).parent) < 0)
                n(root = r).red = false;
            else if (n(pp).left == p)
                n(pp).left = r;
            else
                n(pp).right = r;
            n(r).left = p;
            n(p).parent = r;
        }
        return root;
    }
    i32 rotateRight(i32 root, i32 p) {
        i32 l, pp, lr;
        if (p >= 0 && (l = n(p).left) >= 0) {
            if ((lr = n(p).left = n(l).right) >= 0) n(lr).parent = p;
            if ((pp = n(l).parent = n(p).parent) < 0)
                n(root = l).red = false;
            else if (n(pp).right == p)
                n(pp).right = l;
            else
                n(pp).left = l;
            n(l).right = p;
            n(p).parent = l;
        }
        return root;
    }

    i32 balanceInsertion(i32 root, i32 x) {
        n(x).red = true;
        for (i32 xp, xpp, xppl, xppr;;) {
            if ((xp = n(x).parent) < 0) {
                n(x).red = false;
                return x;
            } else if (!n(xp).red || (xpp = n(xp).parent) < 0) {
                return root;
            }
            if (xp == (xppl = n(xpp).left)) {
                if ((xppr = n(xpp).right) >= 0 && n(xppr).red) {
                    n(xppr).red = false;
                    n(xp).red = false;
                    n(xpp).red = true;
                    x = xpp;
                } else {
                    if (x == n(xp).right) {
                        root = rotateLeft(root, x = xp);
                        xpp = (xp = n(x).parent) < 0 ? -1 : n(xp).parent;
                    }
                    if (xp >= 0) {
                        n(xp).red = false;
                        if (xpp >= 0) {
                            n(xpp).red = true;
                            root = rotateRight(root, xpp);
                        }
                    }
                }
            } else {
                if (xppl >= 0 && n(xppl).red) {
                    n(xppl).red = false;
                    n(xp).red = false;
                    n(xpp).red = true;
                    x = xpp;
                } else {
                    if (x == n(xp).left) {
                        root = rotateRight(root, x = xp);
                        xpp = (xp = n(x).parent) < 0 ? -1 : n(xp).parent;
                    }
                    if (xp >= 0) {
                        n(xp).red = false;
                        if (xpp >= 0) {
                            n(xpp).red = true;
                            root = rotateLeft(root, xpp);
                        }
                    }
                }
            }
        }
    }

    i32 balanceDeletion(i32 root, i32 x) {
        auto red = [&](i32 i) { return i >= 0 && n(i).red; };
        for (i32 xp, xpl, xpr;;) {
            if (x < 0 || x == root) {
                return root;
            } else if ((xp = n(x).parent) < 0) {
                n(x).red = false;
                return x;
            } else if (n(x).red) {
                n(x).red = false;
                return root;
            } else if ((xpl = n(xp).left) == x) {
                if ((xpr = n(xp).right) >= 0 && n(xpr).red) {
                    n(xpr).red = false;
                    n(xp).red = true;
                    root = rotateLeft(root, xp);
                    xpr = (xp = n(x).parent) < 0 ? -1 : n(xp).right;
                }
                if (xpr < 0) {
                    x = xp;
                } else {
                    i32 sl = n(xpr).left, sr = n(xpr).right;
                    if (!red(sr) && !red(sl)) {
                        n(xpr).red = true;
                        x = xp;
                    } else {
                        if (!red(sr)) {
                            if (sl >= 0) n(sl).red = false;
                            n(xpr).red = true;
                            root = rotateRight(root, xpr);
                            xpr = (xp = n(x).parent) < 0 ? -1 : n(xp).right;
                        }
                        if (xpr >= 0) {
                            n(xpr).red = xp < 0 ? false : n(xp).red;
                            if ((sr = n(xpr).right) >= 0) n(sr).red = false;
                        }
                        if (xp >= 0) {
                            n(xp).red = false;
                            root = rotateLeft(root, xp);
                        }
                        x = root;
                    }
                }
            } else {
                if (xpl >= 0 && n(xpl).red) {
                    n(xpl).red = false;
                    n(xp).red = true;
                    root = rotateRight(root, xp);
                    xpl = (xp = n(x).parent) < 0 ? -1 : n(xp).left;
                }
                if (xpl < 0) {
                    x = xp;
                } else {
                    i32 sl = n(xpl).left, sr = n(xpl).right;
                    if (!red(sl) && !red(sr)) {
                        n(xpl).red = true;
                        x = xp;
                    } else {
                        if (!red(sl)) {
                            if (sr >= 0) n(sr).red = false;
                            n(xpl).red = true;
                            root = rotateLeft(root, xpl);
                            xpl = (xp = n(x).parent) < 0 ? -1 : n(xp).left;
                        }
                        if (xpl >= 0) {
                            n(xpl).red = xp < 0 ? false : n(xp).red;
                            if ((sl = n(xpl).left) >= 0) n(sl).red = false;
                        }
                        if (xp >= 0) {
                            n(xp).red = false;
                            root = rotateRight(root, xp);
                        }
                        x = root;
                    }
                }
            }
        }
    }

    std::vector<Node> nodes_;
    std::vector<i32> table_;
    int size_ = 0, threshold_ = 0;
    u32 seq_ = 0;
};

}  // namespace wg
