/*

MIT License

Copyright (c) 2025 Michael Neil (Far Left Lane)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#ifndef _BLOCK_CACHE_H
#define _BLOCK_CACHE_H

#include <ff.h>         //  Obtains integer types
#include <stdbool.h>    //  For bool
#include <diskio.h>     //  Declarations of disk functions


extern void block_cache_init(void);

extern DRESULT block_cache_read_block(BYTE pdrv, LBA_t sector, BYTE *out_data);

extern DRESULT block_cache_write_block(BYTE pdrv, LBA_t sector, const BYTE *in_data);

extern DRESULT block_cache_flush(bool flush_all, bool invalidate_all);

extern void block_cache_print_stats(void);

#endif //   _BLOCK_CACHE_H