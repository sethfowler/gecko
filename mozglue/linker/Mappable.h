/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef Mappable_h
#define Mappable_h

#include <sys/types.h>
#include <pthread.h>
#include "Zip.h"
#include "SeekableZStream.h"
#include "mozilla/RefPtr.h"
#include "zlib.h"

/**
 * Abstract class to handle mmap()ing from various kind of entities, such as
 * plain files or Zip entries. The virtual members are meant to act as the
 * equivalent system functions, with a few differences:
 * - mapped memory is always MAP_PRIVATE, even though a given implementation
 *   may use something different internally.
 * - memory after length and up to the end of the corresponding page is nulled
 *   out.
 */
class Mappable
{
public:
  virtual ~Mappable() { }

  virtual void *mmap(const void *addr, size_t length, int prot, int flags,
                     off_t offset) = 0;

private:
  virtual void munmap(void *addr, size_t length) {
    ::munmap(addr, length);
  }
  /* Limit use of Mappable::munmap to classes that keep track of the address
   * and size of the mapping. This allows to ignore ::munmap return value. */
  friend class Mappable1stPagePtr;

public:
  /**
   * Ensures the availability of the memory pages for the page(s) containing
   * the given address. Returns whether the pages were successfully made
   * available.
   */
  virtual bool ensure(const void *addr) { return true; }

  /**
   * Indicate to a Mappable instance that no further mmap is going to happen.
   */
  virtual void finalize() = 0;

  /**
   * Shows some stats about the Mappable instance.
   * Meant for MappableSeekableZStream only.
   * As Mappables don't keep track of what they are instanciated for, the name
   * argument is used to make the stats logging useful to the reader. The when
   * argument is to be used by the caller to give an identifier of the when
   * the stats call is made.
   */
  virtual void stats(const char *when, const char *name) const { }
};

/**
 * Mappable implementation for plain files
 */
class MappableFile: public Mappable
{
public:
  ~MappableFile() { }

  /**
   * Create a MappableFile instance for the given file path.
   */
  static Mappable *Create(const char *path);

  /* Inherited from Mappable */
  virtual void *mmap(const void *addr, size_t length, int prot, int flags, off_t offset);
  virtual void finalize();

protected:
  MappableFile(int fd): fd(fd) { }

private:
  /* File descriptor */
  AutoCloseFD fd;
};

/**
 * Mappable implementation for deflated stream in a Zip archive
 * Inflates the complete stream into a cache file.
 */
class MappableExtractFile: public MappableFile
{
public:
  ~MappableExtractFile();

  /**
   * Create a MappableExtractFile instance for the given Zip stream. The name
   * argument is used to create the cache file in the cache directory.
   */
  static Mappable *Create(const char *name, Zip *zip, Zip::Stream *stream);

private:
  MappableExtractFile(int fd, char *path)
  : MappableFile(fd), path(path), pid(getpid()) { }

  /**
   * AutoUnlinkFile keeps track or a file name and removes (unlinks) the file
   * when the instance is destroyed.
   */
  struct AutoUnlinkFileTraits: public AutoDeleteArrayTraits<char>
  {
    static void clean(char *value)
    {
      unlink(value);
      AutoDeleteArrayTraits<char>::clean(value);
    }
  };
  typedef AutoClean<AutoUnlinkFileTraits> AutoUnlinkFile;

  /* Extracted file */
  AutoUnlinkFile path;

  /* Id of the process that initialized the instance */
  pid_t pid;
};

class _MappableBuffer;

/**
 * Mappable implementation for deflated stream in a Zip archive.
 * Inflates the mapped bits in a temporary buffer. 
 */
class MappableDeflate: public Mappable
{
public:
  ~MappableDeflate();

  /**
   * Create a MappableDeflate instance for the given Zip stream. The name
   * argument is used for an appropriately named temporary file, and the Zip
   * instance is given for the MappableDeflate to keep a reference of it.
   */
  static Mappable *Create(const char *name, Zip *zip, Zip::Stream *stream);

  /* Inherited from Mappable */
  virtual void *mmap(const void *addr, size_t length, int prot, int flags, off_t offset);
  virtual void finalize();

private:
  MappableDeflate(_MappableBuffer *buf, Zip *zip, Zip::Stream *stream);

  /* Zip reference */
  mozilla::RefPtr<Zip> zip;

  /* Decompression buffer */
  AutoDeletePtr<_MappableBuffer> buffer;

  /* Zlib data */
  z_stream zStream;
};

/**
 * Mappable implementation for seekable zStreams.
 * Inflates the mapped bits in a temporary buffer, on demand.
 */
class MappableSeekableZStream: public Mappable
{
public:
  ~MappableSeekableZStream();

  /**
   * Create a MappableSeekableZStream instance for the given Zip stream. The
   * name argument is used for an appropriately named temporary file, and the
   * Zip instance is given for the MappableSeekableZStream to keep a reference
   * of it.
   */
  static Mappable *Create(const char *name, Zip *zip,
                                         Zip::Stream *stream);

  /* Inherited from Mappable */
  virtual void *mmap(const void *addr, size_t length, int prot, int flags, off_t offset);
  virtual void munmap(void *addr, size_t length);
  virtual void finalize();
  virtual bool ensure(const void *addr);
  virtual void stats(const char *when, const char *name) const;

private:
  MappableSeekableZStream(Zip *zip);

  /* Zip reference */
  mozilla::RefPtr<Zip> zip;

  /* Decompression buffer */
  AutoDeletePtr<_MappableBuffer> buffer;

  /* Seekable ZStream */
  SeekableZStream zStream;

  /* Keep track of mappings performed with MappableSeekableZStream::mmap so
   * that they can be realized by MappableSeekableZStream::ensure.
   * Values stored in the struct are those passed to mmap */
  struct LazyMap
  {
    const void *addr;
    size_t length;
    int prot;
    off_t offset;

    /* Returns addr + length, as a pointer */
    const void *end() const {
      return reinterpret_cast<const void *>
             (reinterpret_cast<const unsigned char *>(addr) + length);
    }

    /* Returns offset + length */
    const off_t endOffset() const {
      return offset + length;
    }

    /* Returns the offset corresponding to the given address */
    const off_t offsetOf(const void *ptr) const {
      return reinterpret_cast<uintptr_t>(ptr)
             - reinterpret_cast<uintptr_t>(addr) + offset;
    }

    /* Returns whether the given address is in the LazyMap range */
    const bool Contains(const void *ptr) const {
      return (ptr >= addr) && (ptr < end());
    }
  };

  /* List of all mappings */
  std::vector<LazyMap> lazyMaps;

  /* Array keeping track of which chunks have already been decompressed.
   * Each value is the number of pages decompressed for the given chunk. */
  AutoDeleteArray<unsigned char> chunkAvail;

  /* Number of chunks that have already been decompressed. */
  size_t chunkAvailNum;

  /* Mutex protecting decompression */
  pthread_mutex_t mutex;
};

#endif /* Mappable_h */
