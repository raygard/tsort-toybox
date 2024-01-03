/* tsort.c - topological sort dependency resolver
 * vi: tabstop=2 softtabstop=2 shiftwidth=2
 *
 * Copyright 2024 Ray Gardner <raygard@gmail.com>
 * License: 0BSD
 *
 * See https://pubs.opengroup.org/onlinepubs/9699919799/utilities/tsort.html

USE_TSORT(NEWTOY(tsort, ">1", TOYFLAG_USR|TOYFLAG_BIN))

config TSORT
  bool "tsort"
  default y
  help
    usage: tsort [FILE]

    Topological sort dependency resolver.

    Read pairs of input strings indicating before/after dependency relationships
    and find an ordering that respects all dependencies. On success output each
    string once to stdout, on failure print error and output cycle pairs.
*/

#include "toys.h"

// A topological sort reads a set of relations, i.e. pairs of
// (predecessor, successor) symbols, and writes a list of unique symbols
// such that each predecessor appears before any of its successors.

// This is Knuth's Algorithm T, Topological sort,
// from The Art of Computer Programming Vol 1, sec. 2.2.3.
//
// The algorithm builds a table of symbols such that each entry has a count of
// its predecessors and a linked list of its successors. When all symbols have
// been read, the symbols with a predecessor count of zero are put into a
// queue. The symbol at the front of the queue is removed and output, and then
// the predecessor count of each of its successors is reduced by one. If any
// of those counts are reduced to zero, that successor's symbol is placed at
// the rear of the queue, and the process is repeated for the next symbol at
// the front of the queue. When the queue is empty, the algorithm checks if
// all the symbols have been output. If not, then there is a loop (cycle) in
// the set of relations. The algorithm walks 'backwards' through predecessors
// to print a cycle.

// The original algo expects relations as pairs drawn from consecutive
// integers starting at 1.  This version assumes arbitrary symbols, so uses a
// hash table to relate the symbols to integers.

// Symbol table, indexed by integers in the hash table below.
// symoffs: offset of the symbol string in the input data
// pq: (Knuth's COUNT aka QLINK) count of predecessors of a symbol
//      and used later as a link in the queue of symbols with no
//      remaining predecessors
// succx: (Knuth's TOP) index into linked list of successors in the successors
//      table, or 0 if no successors
struct symbols {int symoffs, pq, succx;};

// Successors table, indexed by symbols.succx field.
// symbx: (Knuths's SUC) index into symbols table
// next: (Knuth's NEXT) index to next successor, or 0
struct successors {int symbx, next;};

// Hash table to associate symbols with integer indexes to tables 'symbols'
// and 'successors'.  The probe sequence is borrowed from Python dict, using
// the 'perturb' idea to mix in upper bits.
// See https://github.com/python/cpython/blob/main/Objects/dictobject.c
//
// dat is input data area; dat + offs points to symbol to look up.
// hashtbl is the actual hash table. Entries are 0 (unused) or the index into
// symbols table (stbl).
// hashmask is 2**n-1 where n is table size
// symcnt is count of symbols inserted
static int hash_lookup(char *dat, int offs,
        int *hashtbl, int hashmask, int *symcnt, struct symbols *stbl)
{
  int n, probe;
  char *s = dat + offs;
  unsigned perturb = 5381;    // djb2a hash
  while (*s) perturb = perturb * 33 ^ *s++;
  probe = perturb & hashmask;
  while ((n = hashtbl[probe])) {
    if (! strcmp(dat + offs, dat + stbl[n].symoffs)) return n;
    probe = (probe * 5 + 1 + (perturb >>= 5)) & hashmask;
  }
  hashtbl[probe] = ++*symcnt;
  stbl[*symcnt].symoffs = offs;
  return *symcnt;
}

static void do_tsort(int fd, char *name)
{
  struct symbols *stbl;
  struct successors *succlist;
  int i, j, k;
  int nsym, nsym2;
  int *hashtbl, hashmask, symcnt = 0;
  char *pred = 0, *succ;  // pointers to pair symbols (predecessor, successor)
  int npred, nsucc; // indexes of pairs in stbl
  int succ_cnt = 0; // number of successors added to succlist structure
  char *whitespace = " \r\n\t";
  int nleft, front, rear = 0;
  char *dat;        // input area; holds entire input
  off_t plen = 0;
		
  // readfd() ensures dat is at least one byte longer than plen
  // which makes it safe to store a NUL byte after last symbol.
  if (!(dat = readfd(fd, 0, &plen))) return;
  if (plen >= INT_MAX) error_exit("input too big");

  // Make sure input has no NUL bytes
  for (i = 0; i < plen; i++)
    if (!dat[i]) dat[i] = ' ';

  // T1. Initialize. Get number of pairs, allocate tables.
  // Count input symbols (non-unique)
  for (i = nsym = 0; i < plen; nsym++) {
    i += strspn(dat + i, whitespace);
    if (i == plen) break;
    i += strcspn(dat + i, whitespace);
  }
  // nsym == number_of_pairs * 2
  if (nsym & 1) error_exit("bad input (not pairs)");

  // Worst case is every symbol is unique (a b, c d, ...)
  // so need a hash table for at least nsym entries.
  // Hash table must be large enough to load to < 100%
  // Size must be power of 2; use next above (102% of nsym plus 5).
  for (k = 2; k < nsym + nsym / 50 + 5; k *= 2)
    ;
  hashmask = k - 1;
  hashtbl = xzalloc(k * sizeof *hashtbl);

  // Need a symbols structure to hold (nsym + 1) symbols (stbl)
  // plus successors structure (nsym / 2 + 1) successors (succlist).
  // The +1 is to accommodate 1-based indexing.
  stbl = xzalloc((nsym+1) * sizeof(*stbl) + (nsym/2+1) * sizeof(*succlist));
  succlist = (void *)&stbl[nsym+1];

  // re-scan the symbols, enter in hashtbl and stbl.
  for (i = nsym2 = 0; i < plen; nsym2++) {
    // T2. Next relation. Get a relation from input.
    i += strspn(dat + i, whitespace);
    if (i == plen) break;
    j = strcspn(dat + i, whitespace);
    dat[i+j] = 0; // NUL terminate symbol. readfd() ensures safe at buffer end
    if (! (nsym2 & 1)) {
      pred = dat + i;
    } else {
      succ = dat + i;
      npred = hash_lookup(dat, pred-dat, hashtbl, hashmask, &symcnt, stbl);
      nsucc = hash_lookup(dat, i, hashtbl, hashmask, &symcnt, stbl);
      // T3. Record the relation.
      // Slight mod: if self-reference relation, do not record it.
      // (If it's self-reference, the symbol is already in the table now.)
      // Note that this links the most recent into the head of the successor
      // list, reversing their order in the input. If the symbols table kept a
      // tail pointer, they could be kept in the list in original order.
      if (strcmp(pred, succ)) {
        stbl[nsucc].pq++;
        succlist[++succ_cnt].symbx = nsucc;
        succlist[succ_cnt].next = stbl[npred].succx;
        stbl[npred].succx = succ_cnt;
      }
    }
    i += j + 1;
  }
  free(hashtbl);  // Done using hash table.

  // T4. Scan for zeros. Build a queue of symbols w/ no predecessor.
  // Note Knuth's QLINK is our stbl[].pq.
  stbl[0].pq = 0;
  for (k = 1; k <= symcnt; k++) {
    if (! stbl[k].pq) {
      stbl[rear].pq = k;
      rear = k;
    }
  }
  front = stbl[0].pq;

  // T5. Output front of queue.
  nleft = symcnt;   // This will count down as symbols are output.
  while (front) {
    xprintf("%s\n", dat + stbl[front].symoffs);
    nleft--;
    j = stbl[front].succx;
    stbl[front].succx = 0; // Per Knuth 2.2.3 Exercise 23
    // T6. Erase relations. Step through successors of symbol we just output,
    // decreasing the predecessor count of each one. If that count went to
    // zero, put that symbol on the queue.
    while (j) {
      stbl[succlist[j].symbx].pq--;
      if (stbl[succlist[j].symbx].pq == 0) {
        stbl[rear].pq = succlist[j].symbx;
        rear = succlist[j].symbx;
      }
      j = succlist[j].next;
    }
    // T7. Remove the symbol from front of queue.
    front = stbl[front].pq;
  }

  // T8. End, if OK. If any symbols left, there is a loop (cycle)
  // in the associated graph. Find and print a loop.
  if (nleft) {
    int k0, k, kn, knn;
    error_msg("input contains a loop:");

    // Per TAOCP Knuth 2.2.3 Exercise 23, find and print a loop.
    for (k = 1; k <= symcnt; k++) stbl[k].pq = 0;
    // T9. (reuses succx and pq fields) For each symbol, set pq field to
    // refer to a predecessor.
    for (k = 1; k <= symcnt; k++) {
      j = stbl[k].succx;
      stbl[k].succx = 0;
      // T10. Step thru successors, setting corresponding pq field.
      while (j) {
        if (!stbl[succlist[j].symbx].pq)
          stbl[succlist[j].symbx].pq = k;
        j = succlist[j].next;
      }
    }
    // T11. Find a symbol with its pq field set.
    for (k = 1; k <= symcnt; k++)
      if (stbl[k].pq) break;
    //if (k > symcnt) error_exit("bug");  // Not needed if algo is correct...
    // T12. Step thru graph backwards, flagging symbols until start of loop.
    do {
      stbl[k].succx = 1;
      k = stbl[k].pq;
    } while (stbl[k].succx == 0);
    // Per note after T14, the loop links backwards. Reverse the loop.
    // Not sure if this is the cleanest way... (rdg)
    k0 = k;
    kn = stbl[k].pq;
    do {
      knn = stbl[kn].pq;
      stbl[kn].pq = k;
      k = kn;
      kn = knn;
    } while (k != k0);

    // T13. Step through loop, printing symbols.
    do {
      xprintf("%s ->\n", dat + stbl[k].symoffs);
      stbl[k].succx = 0;
      k = stbl[k].pq;
    } while (stbl[k].succx == 1);

    // T14. Print first element of loop again to tie it off.
    xprintf("%s\n", dat + stbl[k].symoffs);
  }
  free(stbl);
}

void tsort_main(void)
{
  loopfiles(toys.optargs, do_tsort);
}
