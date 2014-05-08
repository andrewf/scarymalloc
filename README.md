# MyMalloc

Because prepending "my" to the name of a commonly used construct is always a good
disambiguator. This is a malloc implementation written for educational purposes.
You probably shouldn't use it for anything you hold dear, except eduation.

# Usage
## Or, "how exactly shouldn't I use it?"

First you'll need to build it. I use [`tup`](http://gittup.org/tup/) as my
build system. The commands

    $ tup init
    $ tup upd

will get everything built, including the test programs. If you don't want to
install `tup`, you can do

    $ gcc -shared -fpic -o mymalloc.so mymalloc.c

to build just the `.so`. Then you should have a `mymalloc.so` in your checkout.
To use it with a program,
you need to set the `LD_PRELOAD` environment variable to point to `mymalloc.so`.
The simplest way to do this is by running, in a terminal:

    $ LD_PRELOAD=/path/to/mymalloc.so yourexe

To run it in `gdb`, which is actually a slightly sane idea, you'll need to be
a little more clever. You can use gdb to set environment variables, but by
default when you `run` a program it uses bash to start it, and you probably don't
want bash using mymalloc. Instead, you want to run (from [0]) in the `gdb` shell:

    (gdb) set exec-wrapper env 'LD_PRELOAD=/path/to/mymalloc.so'

This will just set `LD_PRELOAD` for the program you're debugging without any
further complications. This is set up in the `gdb` script `dbsettings`
included in the repo.


# Design

MyMalloc is designed to provide reasonably performant best-fit allocation with
minimal overhead, with coallescing of free blocks. To
accomplish this, free blocks are sorted into one of a single, static array of
linked lists based on their size, and also keep track of their physical
neighbors. All blocks are aligned on 16-byte boundaries,
because x64 seems to think that's important and that's what I'm running on.

In more detail: every memory block has a header and a footer, with the payload,
the memory returned to the caller of `malloc`, between them. The header, `blockHeader`
in the code, contains pointers to the previous and next blocks in the current free
list, if this block is free. There is a size (which contains the payload size,
not including the header and footer) in both the header and footer, so that if
you have one block, you can find the header of the *physically* previous block
for the purpose of coallescing with it. Just find the header address and subtract
`sizeof(blockFooter)` from it, read the size, and then subtract
`footer->size + sizeof(blockHeader)` from the footer address.

I use the nice trick from [1] of storing data in the low bits of my pointers
and sizes. In fact, since they're 16-byte aligned the lowest four bits
of all my pointers and sizes are 0 if no one messes with them, but I only
need these three:

 * One for whether there's a physical previous block (false if this block
   is at the start of a physical chunk returned from sbrk). This is stored
   in `blockHeader.size`
 * One for whether there's a physical next block, in `blockFooter.size`
 * One for whether the block is allocated, in `blockHeader.logicalPrev`

So they all go in the lowest bit of their host value. This turned out to
be relatively useless, since I had to stick a whole other
`size_t` in both the header and footer to get them to be 16-byte aligned,
but at least this way if someone decides to 8-byte align the whole thing it
won't be as miserable.

Anyway, this way every block knows where it is both in the "logical" free
list and the "physical" space of available memory.

Now we have a global array of "buckets", where each bucket is simply the
head of a linked list of free blocks. The range of sizes for blocks stored
in each bucket increases exponentially, up until the last bucket. The last
bucket is a catchall for any blocks that don't fit in the previous buckets.
The function `getBucket` takes a size and returns the bucket that a block
with that size should reside in. These buckets are sorted by size, from least
to greatest. It's basically a very small hash map, with `getBucket` as the
hash function.

To allocate a block, you first call `getBucket` with the size you want to
allocate. Starting with this index, you iterate through the buckets looking
for a block big enough for your allocation. If you find one, unlink it
from the free list, split it if
possible, put the residue back on the free list, and return the front of
the block. Note that this unlinking operation is why the blocks form a doubly-linked list.
If there are no suitable free blocks already, create one at
least large enough with `sbrk`. Split that, re-bucket the residue, and
return as much of the front as necessary.

To free a block given a pointer to the payload, subtract `sizeof(blockHeader)`
from the pointer to get the header. Unset the allocation bit. Use the
"has physical next/prev" bits and the allocation bits of any physically
adjacent blocks to merge with them if possible. Then just stick the resulting
block in the appropriate free list, sorted by size.

An potential avenue for future research is what size of bucket array and kind
of hash function produce the best performance.

If you `#define TESTIT` when compiling mymalloc.c as a program instead of a
library, the resulting program tests a number of assumptions about the
hardware and data types that mymalloc relies on. If you go mucking about
with the internals of mymalloc, it's a good idea to run this and make sure
things are still sane. The tup build system compiles this as `unittest`.

[0] http://www.zyztematik.org/?p=175#comment-1148 "GDB, LD\_PRELOAD and libc"

[1] http://www.cs.cmu.edu/afs/cs/academic/class/15213-f10/www/lectures/17-allocation-basic.pdf This was a nice resource for explaining the basics

I also referred to the man pages for malloc and sbrk extensively, and K&R C.
