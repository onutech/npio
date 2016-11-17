#ifndef NPIO_H_
#define NPIO_H_

/******************************************************************************

Simple header-only routines for reading and writing numpy files. This header
can be used with both C and C++ code. There are no external library
dependencies. Just place the header in your include path to use.

Based on the documentation at
  https://docs.scipy.org/doc/numpy-dev/neps/npy-format.html


License: MIT (see LICENSE.txt)


Limitations:

Can only be used for homogenous arrays. Structured arrays and Object arrays are
not supported.  Structured arrays may be supported at some point in the future.

This code does not conform strictly to the specified format.  It is known to
work with files actually generated by numpy, but there are lots of variations
in the header that are not supported by the minimalist parser in this file.

It is safe to use this with numpy files that may contain pickle objects or
executable code - such files will simply result in an error.

This code assumes that a C char is a single octet.  If you are on a system
that does not satisfy this assumption, you should buy a different system.


Authors:

Vishal (vishal@onutechnology.com)

******************************************************************************/


#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>


/* Version of this header. */
#define NPIO_MAJOR_VERSION 0
#define NPIO_MINOR_VERSION 1

/* Some defaults */
#define NPIO_DEFAULT_MAX_DIM 32

/* Summary of revisions:

0.1  Initial version

*/


/* This struct represents the contents of a numpy file. */
typedef struct 
{
  char   major_version;  /* The npy major version number. */
  char   minor_version;  /* The npy minor version number. */
  size_t header_len;     /* The length of the header in the file. */
  char   *dtype;         /* The unparsed string representation of dtype */
  size_t dim;            /* The number of elements in shape. */
  size_t *shape;         /* The size on each axis. */
  size_t size;           /* The total number of elements.[1] */
  int    fortran_order;  /* Whether or not data is in fortan-ordering. */
  int    little_endian;  /* Whether or not elements are little-endian. */
  int    floating_point; /* Whether data is integral or floating point.*/
  int    is_signed;      /* Whether data is signed or unsigned */
  int    bit_width;      /* The number of bits to this datatype*/
  void   *data;          /* Pointer to contents*/

  /* The following fields are private. */
  int    _fd;        /* File descriptor from which we are loading */  
  void*  _buf;       /* Memory buffer from where we are loading */
  size_t _buf_size;  /* Size of memory buffer from where we are loading */
  char*  _hdr_buf;   /* A buffer for the header, if we are loading from fd */
  size_t _shape_capacity;  /* The space allocated for shape */
  int    _mmapped;   /* Whether we mmapped the data into buf */
  int    _malloced;  /* Whether we allocated the buffer */
  int    _opened;    /* Whether we opened the file descriptor */
} npio_Array;

/*

[1]: the size field is only set when you load a numpy file.  If you
    want to save a numpy file, the size field is ignored by the library
    and the size is computed from the shape. Both the npio_array_size() and
    npio_array_memsize() functions do a full computation of the number of
    elements from the shape, so if you are populating the struct yourself,
    you may want to explicitly set the array->size field to cache it.

*/


/* Compute the total number of elements from the shape. */
static inline size_t npio_array_size(const npio_Array* array)
{
  size_t n = 1, i;
  for (i = 0; i < array->dim; ++i)
    n *= array->shape[i];
  return n;
}


/* Return the memory size in bytes that would be needed by the array data
   Note: this does a full computation of the size from the shape. */
static inline size_t npio_array_memsize(const npio_Array* array)
{
  return npio_array_size(array) * array->bit_width / 8;
}


/* Minimalist parser for python dict in the header. Lots of files that are
valid as per the spec may not be deemed valid by this code. Caveat Emptor. */


/* skip until we see a non-whitespace character. */
static inline const char* npio_ph_skip_spaces_(const char* p, const char* end)
{
  while (p < end && isspace(*p))
    ++p;
  return p;
}


/* append a value to array->shape, reallocating if needed. */
static inline int npio_ph_shape_append_(npio_Array* array, size_t val)
{
  size_t *tmp;
  if (array->dim == array->_shape_capacity)
  {
    array->_shape_capacity *= 2;
    tmp = (size_t*) realloc(array->shape, sizeof(size_t) * array->_shape_capacity);
    if (tmp == 0)
      return ENOMEM;
    array->shape = tmp;
  }
  array->shape[array->dim++] = val;
  return 0;
}


/* Parse a python tuple of integers.  Anything else should fail. */
static inline int npio_ph_parse_shape_(npio_Array* array, const char *start
  , const char *end, const char **where)
{
  const char *p = start, *nbeg, *nend;
  size_t val;
  int err;

  if (p == end)
    return EINVAL;

  if (*p++ != '(')
    return EINVAL;

  array->_shape_capacity = 8;
  array->shape = (size_t*) malloc(sizeof(size_t) * array->_shape_capacity);
  if (array->shape == 0)
    return ENOMEM;

  while (1)
  {
    p = npio_ph_skip_spaces_(p, end);

    nbeg = p;
    while (p < end && *p >= '0' && *p <= '9')
      ++p;
    nend = p;

    if (p == end)
      return EINVAL;

    if (nbeg < nend)  /* trailing comma is allowed in python, so must check */
    {
      val = 0;
      while (nbeg < nend)
        val = (*nbeg++ - '0') + val * 10;
      if ((err = npio_ph_shape_append_(array, val)))
        return err;
    }

    p = npio_ph_skip_spaces_(p, end);
    if (p == end)
      return EINVAL;
    if (*p == ',')
      ++p;
    else if (*p == ')')
    {
      ++p;
      break;
    }
    else
      return EINVAL;
  }

  *where = p;
  array->size = npio_array_size(array);
  return 0;
}


/*

Parse a dtype for homogeneous arrays.

The Numpy specification says that the dtype can be any Python object that would
serve as a valid argument to numpy.dtype(). Right now we restrict the dtype to
match strings of the form 'EDN' where E can be '<' or '>' for the endianness, D
can be 'i', 'f' or 'u' for the c-type and N can be 1, 2, 4, or 8. All others we
reject. If we really want support for structured data (tables), then we need a
better parser and a single-header minimalist solution may not be appropriate.

Arguments:
  dtype is the null-terminated string value of the dtype in the header.

Return:
  A parsed type if we understand the dtype, otherwise npio_unknown.

*/
static inline int npio_ph_parse_dtype_(npio_Array* array)
{
  const char* dtype = array->dtype;

  if (strlen(dtype) != 3)
    return ENOTSUP;
  
  switch (dtype[0])
  {
    case '<': array->little_endian = 1; break;
    case '>': array->little_endian = 0; break;
    default : return ENOTSUP;
  }

  switch (dtype[1])
  {
    case 'i':
      array->is_signed = 1;
      array->floating_point = 0;
      break;

    case 'u':
      array->is_signed = 0;
      array->floating_point = 0;
      break;

    case 'f':
      array->is_signed = 1;
      array->floating_point = 1;
      break;

    default:
      return ENOTSUP;  
  }

  switch (dtype[2])
  {
    case '1': array->bit_width = 8; break;
    case '2': array->bit_width = 16; break;
    case '4': array->bit_width = 32; break;
    case '8': array->bit_width = 64; break;
    default: return ENOTSUP;
  }

  return 0;
}


static inline int npio_ph_is_quote_(char p)
{
  return (p == '\'' || p == '"');
}


/* parse a python dictionary containing the specific keys and value we expect */
static inline int npio_ph_parse_dict_(npio_Array* array, const char* start, const char* end)
{
  int err;
  char open_quote;
  const char *p = start;
  const char *dtbeg, *dtend;
  size_t dtsz;
  enum {k_descr, k_shape, k_fortran_order} key;

  if (p >= end)
    return EINVAL;
  
  if (*p++ != '{')
    return EINVAL;

  /* Go through all key-value pairs. */
  while (1)
  {
    p = npio_ph_skip_spaces_(p, end);
    if (p >= end)
      return EINVAL;

    /* are we done? */
    if (*p == '}')
    {
      ++p;
      break;
    }

    /* Expect the open quote of a key */
    if (!npio_ph_is_quote_(open_quote = *p++))
      return EINVAL;
    
    /* Check for one of the three possible keys */
    if (p + 5 < end && memcmp(p, "descr", 5) == 0)
    {
      key = k_descr;
      p += 5;
    }
    else if (p + 5 < end && memcmp(p, "shape", 5) == 0)
    {
      key = k_shape;
      p += 5;
    }
    else if (p + 13 < end && memcmp(p, "fortran_order", 13) == 0)
    {
      key = k_fortran_order;
      p += 13;
    }
    else
      return EINVAL;

    /* Expect the close quote of the key */
    if (p >= end || *p++ != open_quote)
      return EINVAL;

    /* Get to the colon */
    p = npio_ph_skip_spaces_(p, end);
    if (p >= end || *p++ != ':')
      return EINVAL;

    /* skip any more spaces */
    p = npio_ph_skip_spaces_(p, end);
    if (p == end)
      return EINVAL;

    switch (key)
    {
      case k_descr:
        if (!npio_ph_is_quote_(open_quote = *p++))
          return EINVAL;
        dtbeg = p;  
        while (p < end && *p != open_quote)
          ++p;
        dtend = p;  
        if (p == end)
          return EINVAL;
        ++p;  
        dtsz = dtend - dtbeg;  
        array->dtype = (char*) malloc(dtsz + 1);
        memcpy(array->dtype, dtbeg, dtsz);
        array->dtype[dtsz] = 0;
        break;

      case k_fortran_order:
        if (p + 4 < end && memcmp(p, "True", 4) == 0)
        {
          array->fortran_order = 1;
          p += 4;
        }
        else if (p + 5 < end && memcmp(p, "False", 5) == 0)
        {
          array->fortran_order = 0;
          p += 5;
        }
        else
          return EINVAL;
        break;

      case k_shape:
        if ((err = npio_ph_parse_shape_(array, p, end, &p)))
          return err;
        break;  
    }

    /* skip any spaces after end of key : value */
    p = npio_ph_skip_spaces_(p, end);

    if (p == end)
      return EINVAL;

    /* grab that separating comma */
    if (*p == ',')
      ++p;

    /* next iteration takes care of any nonsense that might happen here! */
  }

  /* Parse the (very restricted) numpy dtype and return */
  return npio_ph_parse_dtype_(array);
}



/* Initialize the struct so that we can cleanup correctly. Also provide
defaults for npy_save
*/
static inline void npio_init_array(npio_Array* array)
{
  array->major_version = 1;
  array->minor_version = 0;
  array->header_len = 0;
  array->dtype = 0;
  array->dim = 0;
  array->shape = 0;
  array->size = 0;
  array->fortran_order = 0;
  array->little_endian = (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
  array->floating_point = 1;
  array->is_signed = 1;
  array->bit_width = 32;
  array->data = 0;
  array->_fd = 0;
  array->_buf = 0;
  array->_buf_size = 0;
  array->_hdr_buf = 0;
  array->_shape_capacity = 0;
  array->_mmapped = 0;
  array->_malloced = 0;
}


/*

Release all resources associated with an Array that was initialized with
npio_init_array and then populated via one of the npio_load_* functions.

You should not call this if you manually populated the Array structure for
writing out data that you allocated yourself.

Note: this function does not free any memory associated with the struct itself.

Arguments:
  array: a previously loaded array.
*/
static inline void npio_free_array(npio_Array* array)
{
  if (array->dtype)
  {
    free(array->dtype);
    array->dtype = 0;
  }

  if (array->shape)
  {
    free(array->shape);
    array->shape = 0;
  }

  if (array->_mmapped)
  {
    munmap(array->_buf, array->_buf_size);
    array->_buf = 0;
  }

  if (array->_fd >= 0)
  {
    close(array->_fd);
    array->_fd = -1;
  }

  if (array->_hdr_buf)
  {
    free(array->_hdr_buf);
    array->_hdr_buf = 0;
  } 
}


/*
Check the magic number, the format version and gets the HEADER_LEN field.
The prelude should have atleast 10 characters for version 1 and 12 characters
for version 2.
*/
static inline int npio_load_header_prelude_(char* p, npio_Array* array, char** end)
{
  /* assert magic string. Basic size check was done above. */
  if (memcmp(p, "\x93NUMPY", 6) != 0)
    return EINVAL;
  p += 6;

  /* get the version numbers */
  array->major_version = *p++;
  array->minor_version = *p++;

  /* get the header length. Version 1 uses 2 bytes, version 2 uses 4 bytes. */
  switch (array->major_version)
  {
    case 1:
      array->header_len = p[0] + (p[1] << 8);
      p += 2;
      break;

    case 2:
      array->header_len = p[0]
        + (p[1] << 8)
        + (p[2] << 16)
        + (p[3] << 24);
      p += 4;  
      break;
    
    default:
      return ENOTSUP;
  }
  *end = p;
  return 0;
}


/* Load the header from a pointer to (partial) file data in memory. */
static inline int npio_load_header_mem4(void* p_, size_t sz
  , npio_Array* array, size_t max_dim)
{
  int err;
  char* p = (char*) p_;
  char *end = p + sz;

  /* sanity check, to avoid some checks a bit later. */
  if (sz < 16)
    return EINVAL;

  /* Store this buffer address for load_data */
  if (!array->_mmapped)
  {
    array->_buf = p;
    array->_buf_size = sz;
  }

  if ((err = npio_load_header_prelude_(p, array, &p)))
    return err;

  /* Ensure that the header_len doesn't make us go out of bounds */
  if (p + array->header_len < end)
    end = p + array->header_len;

  /* Parse the header and return */
  return npio_ph_parse_dict_(array, p, end);
}


/* Same as above, with default max_dim */
static inline int npio_load_header_mem(void *p, size_t sz, npio_Array* array)
{
  return npio_load_header_mem4(p, sz, array, NPIO_DEFAULT_MAX_DIM);
}


/* Loads the header using read calls instead of mmap. */
static inline int npio_load_header_fd_read_(int fd, npio_Array* array, size_t max_dim)
{
  /* Read just enough to know how much more we need to read */
  char prelude[12];
  char *end;
  int nr, err;
  size_t prelude_size;

  nr = read(fd, prelude, sizeof(prelude));
  if (nr < (int) sizeof(prelude))
    return errno;

  if ((err = npio_load_header_prelude_(prelude, array, &end)))
    return err;

  /* Keep track of how many bytes of prelude were present */
  prelude_size = end - prelude;

  /* We suppose here that each dimension in shape should not take more than 20
  characters to estimate a limit on the header_len. Admitted, this is sloppy. */
  if (array->header_len > 1024 + max_dim * 20)
    return ERANGE;

  /* We stick the prelude back together with the rest of the header */
  if ((array->_hdr_buf = (char*) malloc(sizeof(prelude) + array->header_len)) == 0)
    return ENOMEM;
  memcpy(array->_hdr_buf, prelude, sizeof(prelude));

  /* Now read in the rest of the header */
  nr = read(fd, array->_hdr_buf + sizeof(prelude), array->header_len);

  /* Parse the header */
  return npio_ph_parse_dict_(array, array->_hdr_buf + prelude_size, end);
}


/*
Load the header of a numpy file.  If successful, you may call npio_load_data
subsequently to actually obtain the array elements.  Finally you must call
npio_free_array to release all associated resources, even if there was an
error while loading.

Arguments:
  fd: file descriptor of a file in numpy format.
  array: pointer to an Array struct that will be populated (partially) on
    return. The data is not loaded.  You must have called npio_init_array
    on array prior to calling this function.
  max_hdr_size: as a security measure, return an error if the header size
    is larger than this limit.

Return:
  0 on success.
  EINVAL   the file is not a valid numpy file, or we failed to parse.
  ENOTSUP  unsupported npy version
  ERANGE   header exceeded max_hdr_size
  Other errno codes if file could not be accessed etc.
*/
static inline int npio_load_header_fd3(int fd, npio_Array* array, size_t max_dim)
{
  ssize_t file_size;
  char *p;

  /* Store the file descriptor for load_data */
  if (!array->_opened)
    array->_fd = fd;
  
  /* Get the file size in preparation to mmap */
  file_size = lseek(fd, 0, SEEK_END);

  /* If we could not lseek, fallback on read. */
  if (file_size < 0)
    return npio_load_header_fd_read_(fd, array, max_dim);

  /* map-in the file */
  p = (char*) mmap(0, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    return npio_load_header_fd_read_(fd, array, max_dim);

  array->_mmapped = 1;
  array->_buf = p;
  array->_buf_size = file_size;

  return npio_load_header_mem4(p, file_size, array, max_dim);
}


/* Same as above, with default max_dim */
static inline int npio_load_header_fd(int fd, npio_Array* array)
{
  return npio_load_header_fd3(fd, array, NPIO_DEFAULT_MAX_DIM);
}


/* Load the header of a numpy array from a file. The data is not explicitly
   loaded (although it may be mapped into memory if the specified path is
   mappable). */
static inline int npio_load_header3(const char* filename, npio_Array* array
  , size_t max_dim)
{
  int fd, err;
  fd = open(filename, O_RDONLY);
  if (fd < 0)
    return errno;
  array->_fd = fd;
  array->_opened = 1;
  err = npio_load_header_fd3(fd, array, max_dim);

  /* we don't need to hang on the fd if we managed to map */
  if (array->_mmapped)
  {
    close(fd);
    array->_fd = -1;
    array->_opened = 0;
  }
  return err;
}


/* Same as above, with a default max_dim */
static inline int npio_load_header(const char* filename, npio_Array* array)
{
  return npio_load_header3(filename, array, NPIO_DEFAULT_MAX_DIM);
}


/* Swap two bytes, used for endian conversion below. */
static inline void npio_swap_bytes_(char* p1, char* p2)
{
  char tmp;
  tmp = *p2;
  *p2 = *p1;
  *p1 = tmp;
}


/*

Reverse the bytes of each element in the array to go from little to big or big
to little endian. This is a simple non-optimized implementation for the sake of
portability and ease-of-compilation of this code. Performance will be quite
poor.

Note: assumption: 1 byte == 1 octet.

Arguments:
  n: the number of elements in the array
  bit_width: number of bits per element, with 8 bits == 1 byte
  data: pointer to data.

*/
static inline int npio_swap_bytes(size_t n, size_t bit_width, void* data)
{
  size_t i;
  char *p = (char*) data;

  if (bit_width == 8)
    return 0;

  switch (bit_width)
  {
    case 16:
      for (i = 0; i < n; ++i)
      {
        npio_swap_bytes_(p, p + 1);
        p += 2;
      }
      return 0;

    case 32:
      for (i = 0; i < n; ++i)
      {
        npio_swap_bytes_(p, p + 3);
        npio_swap_bytes_(p + 1, p + 2);
        p += 4;
      }
      return 0;

    case 64:
      for (i = 0; i < n; ++i)
      {
        npio_swap_bytes_(p, p + 7);
        npio_swap_bytes_(p + 1, p + 6);
        npio_swap_bytes_(p + 2, p + 5);
        npio_swap_bytes_(p + 3, p + 4);
        p += 8;
      }
      return 0;

    default:
      return ENOTSUP;
  }
}


/*
Load the array data, having previously read a header via npio_load_header.

If swap_bytes is true, the loaded data is converted to host endian.

*/

static inline int npio_load_data2(npio_Array* array, int swap_bytes)
{
  /* These macros work on both GCC and CLANG without an additional include.
     Right now we do not support any other endianness than big and little.
     Numpy also only supports these two ('<' and '>'). */
  static const int little_endian = (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
  size_t data_offset;
  size_t sz;
  ssize_t nr;

  /* Check that the header_len matches the alignment requirements
     of the format */
  data_offset = array->header_len + 6 + (array->major_version == 1 ? 4 : 6);
  if (data_offset % 16)
    return EINVAL;

  if (array->_mmapped)
  {
    /* Check that the indicated data is within the mapped bounds. */
    /* We are also checking here that there is no trailing data. */
    if (data_offset + npio_array_memsize(array) != array->_buf_size)
      return EINVAL;

    /* Set the data pointer to the start of data */
    array->data = (char*) array->_buf + data_offset;
  }
  else
  {
    /* we must allocate memory and read in the data */
    sz = array->size * array->bit_width / 8;
    if ((ssize_t) sz < 0)
      return ERANGE;
    array->data = malloc(sz);
    if (!array->data)
      return ENOMEM;
    nr = read(array->_fd, array->data, sz);
    if (nr != (ssize_t) sz)
      return errno;
  }

  /* Swap bytes if necessary */ 
  if (swap_bytes && little_endian != array->little_endian)
  {
    array->little_endian = little_endian;
    return npio_swap_bytes(array->size, array->bit_width, array->data);
  }

  return 0;
}


/* Same as above, but always swaps the byte order to match host. */
static inline int npio_load_data(npio_Array* array)
{
  return npio_load_data2(array, 1);
}


/*
Load a numpy file.

Arguments:
  fd: file descriptor of a file in numpy format.
  array: pointer to an Array struct that will be populated on success.

Return:
  0 on success, error code otherwise.
*/
static inline int npio_load_fd3(int fd, npio_Array* array, size_t max_dim)
{
  int err;
  
  /* First load the header. */
  if ((err = npio_load_header_fd3(fd, array, max_dim)))
    return err;

  return npio_load_data(array);
}


/*
Load a numpy file.

Arguments:
  filename: name of the file to be loaded.
  array: an Array struct that will be populated on return.  You should not
    have called npio_init_array on the array. On success, you can access
    the header information and data in array.  When done with it, you must
    call npio_free_array() on the array to cleanup.

Return:
  0 on success, error code otherwise.
*/

static inline int npio_load3(const char* filename, npio_Array* array
  , size_t max_dim)
{
  int fd, err;
  fd = open(filename, O_RDONLY);
  if (fd < 0)
    return errno;
  err = npio_load_fd3(fd, array, max_dim);
  close(fd);
  return err;
}


/* Same as above, but with a reasonable default for the max_hdr_size
safety parameter. */
static inline int npio_load(const char* filename, npio_Array* array)
{
  /* This covers all version-1 files and should not be in any danger
  of bringing down the calling code. */
  return npio_load3(filename, array, 65536);
}


/* Load an numpy array from a memory buffer.  The contents point to the
   buffer and are not copied. */
static inline int npio_load_mem4(void *p, size_t sz, npio_Array* array
  , size_t max_dim)
{
  int err;
  if ((err = npio_load_header_mem4(p, sz, array, max_dim)))
    return err;
  return npio_load_data(array);
}


/* Same as above, with a default for max_dim. */
static inline int npio_load_mem(void *p, size_t sz, npio_Array* array)
{
  return npio_load_mem4(p, sz, array, NPIO_DEFAULT_MAX_DIM);
}


/* Prepare a numpy header in the designated memory buffer. On success, zero is
returned and out is set to 1 beyond the last written byte of the header. */
static inline int npio_save_header_mem(void* p, size_t sz, const npio_Array* array
  , void **out)
{
  size_t i, hdr_len;
  char* hdr_buf = (char*) p;
  char* hdr_end = hdr_buf + sz;
  char* hdr = hdr_buf;

  /* This is the absolute minimum space for the header the way we write it. */
  if (sz < 64)
    return ERANGE;

  memcpy(hdr, "\x93NUMPY\x1\x0", 8);
  hdr = hdr_buf + 8 + 2; /* leave 2 bytes for header_len, to be filled later*/
  hdr += sprintf(hdr, "{\"descr\": \"%c%c%d\", "
    , array->little_endian ? '<' : '>'
    , array->floating_point ? 'f' : array->is_signed ? 'i' : 'u'
    , array->bit_width / 8);
  /* There is no way hdr can overflow at this point! */
  
  hdr += sprintf(hdr, "\"fortran_order\": %s, "
    , array->fortran_order ? "True" : "False");
  /* Again, no need to check for overflow here. */

  hdr += sprintf(hdr, "\"shape\": (");
  for (i = 0; i < array->dim; ++i)
  {
    hdr += snprintf(hdr, hdr_end - hdr, "%ld, ", array->shape[i]);
    if (hdr >= hdr_end - 3)
      return ERANGE;
  }
  hdr += sprintf(hdr, ")} ");  /* hence the -3 above. */

  /* insert pad spaces */
  while (hdr < hdr_end && ((hdr - hdr_end) % 16))
    *hdr++ = ' ';

  /* check that we still have space */
  if ((hdr - hdr_end) % 16)
    return ERANGE;

  /* terminate with a \n.  The npy specification is vague on this. One
  interpretation is that the \n can occur first and then be followed by
  spaces, but that causes "IndentationError: unexpected indent" from the
  python loader. */
  hdr[-1] = '\n';

  /* Fill in the header_len field */
  hdr_len = hdr - hdr_buf - 10;
  hdr_buf[8] = hdr_len & 0xff;
  hdr_buf[9] = hdr_len >> 8;

  *out = hdr;
  return 0;
}


/*
Save a numpy file.

Arguments:
  fd: file descriptor of file to be saved to.
  array: array to be saved.

Return:
  0 on success.
  ERANGE: the array has too many dimensions.
  Other IO error from the OS.
*/
static inline int npio_save_fd(int fd, const npio_Array* array)
{
  /*
  We save in version 1 for now, so no need for dynamic allocation.
  hdr_buf should be no smaller than 128 and should be divisible by 16.
  */
  char hdr_buf[65536];
  void *end;
  char* hdr;
  int err;
  ssize_t nw;
  size_t sz;
  
  if ((err = npio_save_header_mem(hdr_buf, sizeof(hdr_buf), array, &end)))
    return err;

  hdr = (char*) end;
  nw = write(fd, hdr_buf, hdr - hdr_buf);
  if (nw < hdr - hdr_buf)
    return errno;

  /* Ok, now write out the data */
  sz = npio_array_memsize(array);
  if ((ssize_t) sz < 0)
    return ERANGE;  /* We can break it up into multiple writes,
                       but this is pretty damn large and will surely
                       exceed filesystem limits! */
  nw = write(fd, array->data, sz);
  if (nw != (ssize_t) sz)
    return errno;

  /* all done */
  return 0; 
}


/*
Save a numpy file.

Arguments:
  filename: name of the file to be saved to.
  array: the array to be saved.

Return:
  0 on success, error code otherwise.
*/
static inline int npio_save(const char* filename, const npio_Array* array)
{
  int fd, err;
  fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return errno;
  err = npio_save_fd(fd, array);
  close(fd);
  return err;  
}


#ifdef __cplusplus

// Convenience wrappers for C++

// Enable additional convenience functions for C++11
#define NPIO_CXX11 __cplusplus >= 201103L

// You must define this macro if you want the C++ API to use exceptions instead
// of return values.
#ifdef NPIO_CXX_ENABLE_EXCEPTIONS
  #include <typeinfo>
  #include <system_error>
#endif

#if NPIO_CXX11
  #include <initializer_list>
#endif


namespace npio
{


// Not relying on C++11 type_traits for compatibilty with legacy code-bases.


//For integral types.
template <class T>
struct Traits
{
  static const bool is_signed = T(-1) < T(0);
  static const bool floating_point = false;
  static const size_t bit_width = sizeof(T) * 8;
  static const char spec = is_signed ? 'i' : 'u';
};


template <>
struct Traits<float>
{
  static const bool is_signed = true;
  static const bool floating_point = true;
  static const size_t bit_width = 32;
  static const char spec = 'f';
};


template <>
struct Traits<double>
{
  static const bool is_signed = true;
  static const bool floating_point = true;
  static const size_t bit_width = 64;
  static const char spec = 'f';
};


// Save the array specicied by nDim, shape and data to a file descriptor
// opened for writing.
template <class T>
int save(int fd, size_t nDim, const size_t *shape, const T* data)
{
  npio_Array array;
  npio_init_array(&array);
  array.dim = nDim;
  array.shape = (size_t*) shape;
  array.floating_point = Traits<T>::floating_point;
  array.is_signed = Traits<T>::is_signed;
  array.bit_width = Traits<T>::bit_width;
  array.data = (char*) data;

  return npio_save_fd(fd, &array);
}


#if NPIO_CXX11
// Same as above, but with initializer_list for convenience
template <class T>
int save(int fd, std::initializer_list<size_t> shape, const T* data)
{
  return save(fd, shape.size(), shape.begin(), data);
}
#endif


// Save the array specified by nDim, shape and data to the specified file.
template <class T>
int save(const char* fn, size_t nDim, const size_t *shape, const T* data)
{
  int fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return errno;
  int ret = save(fd, nDim, shape, data);
  close(fd);
  return ret;
}


#if NPIO_CXX11
// Same as above, but with initializer_list
template <class T>
int save(const char* fn, std::initializer_list<size_t> shape, const T* data)
{
  return save(fn, shape.size(), shape.begin(), data);
}
#endif


// Simple untyped class wrapper for npio_load*
class Array
{
  private:
    npio_Array array;

    #ifndef NPIO_CXX_ENABLE_EXCEPTIONS
      int err;
    #endif


  public: 
    Array(const char* filename, size_t max_dim = NPIO_DEFAULT_MAX_DIM)
    {
      npio_init_array(&array);
      #ifdef NPIO_CXX_ENABLE_EXCEPTIONS
        if (int err = npio_load3(filename, &array, max_dim))
          throw std::system_error(err, std::system_category());
      #else
        err = npio_load3(filename, &array, max_dim);
      #endif
    }


    Array(int fd, size_t max_dim = NPIO_DEFAULT_MAX_DIM)
    {
      npio_init_array(&array);
      #ifdef NPIO_CXX_ENABLE_EXCEPTIONS
        if (int err = npio_load_fd3(fd, &array, max_dim))
          throw std::system_error(err, std::system_category());
      #else
        err = npio_load_fd3(fd, &array, max_dim);
      #endif
    }


    Array(void *p, size_t sz, size_t max_dim = NPIO_DEFAULT_MAX_DIM)
    {
      npio_init_array(&array);
      #ifdef NPIO_CXX_ENABLE_EXCEPTIONS
        if (int err = npio_load_mem4(p, sz, &array, max_dim))
          throw std::system_error(err, std::system_category());
      #else
        err = npio_load_mem4(p, sz, &array, max_dim);
      #endif
    }


    // Read-only accessors

    #ifndef NPIO_CXX_ENABLE_EXCEPTIONS
      // Get any error that occurred during construction.  You must check this
      // if you are not using exceptions.
      int error() const { return err; }
    #endif

    // Get the total number of elements
    size_t size() const { return array.size; }

    // Get the dimensionality of the array
    size_t dim() const { return array.dim; }

    // Whether the array is in fortran order
    bool fortran_order() const { return array.fortran_order; }

    // Whether the array is in little endian order
    bool little_endian() const { return array.little_endian; }

    // Whether the data type is a float type.
    bool floating_point() const { return array.floating_point; }

    // Whether the data is signed.
    bool is_signed() const { return array.is_signed; }

    // Number of bits per element.
    size_t bit_width() const { return array.bit_width; }

    // The size along each dimension.
    const size_t* shape() const { return array.shape; }

    // The raw data pointer.
    const void* data() const { return array.data; }

    // The major and minor version of the loaded file's format.
    char major_version() const { return array.major_version; }
    char minor_version() const { return array.minor_version; }


    // Some convenience functions


    // Get the size along ith dimension.  Implicitly all dimensions are 1
    // beyond the dimensionality of the array.
    size_t shape(size_t i)
    {
      if (i < array.dim)
        return array.shape[i];
      else
        return 1;   
    }


    // Returns whether the underlying data is of the specified type T.
    template <class T>
    bool isType() const
    {      
      return Traits<T>::floating_point == array.floating_point
        && Traits<T>::is_signed == array.is_signed
        && Traits<T>::bit_width == array.bit_width;
    }


    // Get a typed pointer to the data.  If the type does not match the
    // underlying data, throws a bad_cast if exceptions are enabled.
    template <class T>
    T* get() const
    {
      if (!isType<T>())
      {
        #ifdef NPIO_CXX_ENABLE_EXCEPTIONS
          throw std::bad_cast();
        #else
          return 0;
        #endif
        return (T*) array.data;
      }
    }


    #if NPIO_CXX11
      template <class T>
      class ValueRange
      {
        T* _begin;
        T* _end;
        
        ValueRange(T* begin, T* end)
          : _begin(begin)
          , _end(end)
        {}
        
        friend class Array;

        public:
          ~ValueRange() = default;
          T* begin() const { return _begin; }
          T* end() const { return _end; }
      };
    
      // For C++11 range-based for loops
      template <class T>
      ValueRange<T> values() const
      {
        if (isType<T>())
          return ValueRange<T>((T*) array.data, (T*) array.data + array.size);
        else
        {
          #ifdef NPIO_CXX_ENABLE_EXCEPTIONS
            throw std::bad_cast();
          #else
            return ValueRange<T>(0, 0);
          #endif
        }
      }
    
    #endif
    

    // Save the array back to file
    int save(const char* filename)
    {
      return npio_save(filename, &array);
    }


    // Save the array back to fd
    int save(int fd)
    {
      return npio_save_fd(fd, &array);
    }


    ~Array()
    {
      npio_free_array(&array);
    }
};



}  // namespace npio
#endif





#endif