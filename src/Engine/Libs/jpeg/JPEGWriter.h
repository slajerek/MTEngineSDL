#ifndef AJS_JPEG_WRITER_H
#define AJS_JPEG_WRITER_H

#include <cstdio>
#include <cstdlib>
#include <cassert>

extern "C" {
    #include "jpeglib.h"
    #include "jerror.h"
}

#include <vector>
#include <string>
#include <stdexcept>

#include "JPEG.h"
#include "SYS_FileUtf8.h"

/// \class JPEGWriter JPEGWriter.h
/// Write a JPEG image to file using \c libjpeg.
/// Thread-safe if multiple threads do not access the same JPEGWriter at once.
/// Writes directly from the caller's memory, minimizing copying.
class JPEGWriter {
    public:
        
    /// Initialize the libjpeg structures.
    JPEGWriter();
    
    /// Free the libjpeg structures.
    ~JPEGWriter();

    /// Set up the JPEG header,
    void header(const unsigned width, const unsigned height, 
                const unsigned components, const JPEG::ColorSpace colorSpace);
    
    /// Set the quality setting, in the range of zero (lowest) to 100 (highest).
    /// If \c forceBaseline is true, then compatibility with different JPEG 
    /// readers is increased at the expense of increased file size of very-low 
    /// quality (under 25) images.
    void setQuality(const unsigned value, const bool forceBaseline = false);
    
    /// Set the time/quality tradeoff.
    void setTradeoff(const JPEG::TimeQualityTradeoff value);

    /// Write a JPEG image given by the row iterator \c rows to the file at \c path.
    ///
    /// A RowPtrIter should act basically like an \c unsigned char**:
    /// 
    /// - It must dereference to the start of a chunk of memory at least 
    ///   \c width * \c components bytes long.
    /// - It should be incrementable \c height times.
    /// - It will be pre-incremented immediately after each row is written out.
    /// 
    /// Normal pointers, iterators into a std::vector<unsigned char*>, and related
    /// things will all work as you expect.
    /// 
    /// A normal usage would be something like:
    /// \code
    /// writer.header(width, height, 3, JPEG::COLOR_RGB);
    /// unsigned char** rows = new unsigned char*[height];
    /// for (int i = 0; i < height; ++i)
    ///     rows[i] = new unsigned char[width];
    /// writer.write("myfile.jpg", rows);
    /// \endcode
    /// Of course, the equivalent code using \c std::vector is highly recommended. 
    ///
    /// If you have only a row at a time, then something like the following
    /// will also work and be more efficient:
    /// \code
    /// struct InputRowIter {
    ///     InputRowIter(some input source, const unsigned size):
    ///         buffer(size) {
    ///         ... fill the buffer with one row's worth of pixels ...
    ///     }
    ///
    ///     unsigned char* operator*() {
    ///         return &buffer[0];
    ///     }
    ///     
    ///     void operator++() {
    ///         ... fill the buffer with one row's worth of pixels ...
    ///     }
    ///     
    ///     std::vector<unsigned char> buffer;
    /// };
    /// 
    /// InputRowIter rowIter(some input source, width * components);
    /// writer.header(width, height, 3, JPEG::COLOR_RGB);
    /// writer.write("myfile.jpg", rowIter);
    /// \endcode
    template <typename RowPtrIter>
    void write(const std::string& path, RowPtrIter rows);

	template <typename RowPtrIter>
    void write(unsigned char ** outbuffer, unsigned long * outsize, RowPtrIter rows);

    /// Set chroma subsampling: 0 = 4:4:4 (no subsampling), 1 = 4:2:0.
    ///
    /// MUST be called AFTER header(). cinfo.comp_info does not exist until
    /// jpeg_set_defaults() allocates it, and jpeg_set_defaults() is the last
    /// thing header() does -- calling this before header() dereferences null.
    /// jpeg_set_defaults() also writes the sampling factors itself, so an
    /// earlier value would be overwritten even if the array existed.
    ///
    /// The same applies to setQuality() and setTradeoff(): everything between
    /// header() and beginWrite() is the libjpeg parameter-override window, and
    /// nothing in that group may precede header(). setQuality() before
    /// header() is silently reset to 75 by jpeg_set_defaults().
    void setChromaSubsampling(const int mode);

    /// Suppress (or restore) the JFIF APP0 header libjpeg writes by default.
    ///
    /// Matters for Exif output: jpeg_start_compress emits JFIF APP0 first
    /// (jcmarker.c:537), so a subsequent Exif APP1 lands *second*, while the
    /// Exif specification requires APP1 immediately after SOI. Most readers
    /// cope with JFIF-then-Exif, but a caller producing a conformant Exif file
    /// should pass false. Same window as setChromaSubsampling: after header(),
    /// before beginWrite().
    void setWriteJfifHeader(const bool enabled);

    /// --- Split write, for callers that need to emit APP markers ----------
    ///
    /// Markers must land between jpeg_start_compress and the first scanline,
    /// which the one-shot write() above runs back to back. Call order:
    ///
    ///     header() -> setQuality()/setTradeoff()/setChromaSubsampling()
    ///             -> beginWrite() -> writeMarker()* -> writeRows() -> endWrite()
    ///
    /// Open the destination file and start compression.
    void beginWrite(const std::string& path);

    /// Emit one APPn marker. Valid only between beginWrite() and the first
    /// writeRows(); emitted in call order (Exif expects APP1 first, ICC APP2
    /// after).
    ///
    /// Payloads over 65533 bytes THROW rather than being chunked. Chunking is
    /// marker-type-specific -- ICC APP2 carries "ICC_PROFILE\0" plus sequence
    /// and count bytes, Extended XMP uses a GUID and offsets, and Exif APP1
    /// has no chunking mechanism at all -- so a function handed only a marker
    /// number and bytes cannot do it correctly. A caller needing ICC chunking
    /// emits the segments itself as repeated writeMarker() calls.
    void writeMarker(const int marker, const unsigned char* data, const unsigned len);

    /// Write all scanlines from the row iterator (see write() for the
    /// RowPtrIter contract).
    template <typename RowPtrIter>
    void writeRows(RowPtrIter rows);

    /// Finish compression and close the file. Idempotent.
    void endWrite();

    /// Get warnings generated by libjpeg since the last call to header().
    /// Separate warnings are separated by a newline.
    const std::string& warnings() const;

#ifndef DOXYGEN_SHOULD_SKIP_THIS
    /// libjpeg wants us to exit because of an error (private).
    void error_exit();
    
    /// libjpeg wants us to output a message because of an error (private).
    void output_message();
#endif
    
    private:
    // Disallow copying
    JPEGWriter(const JPEGWriter& other);
    JPEGWriter& operator=(const JPEGWriter& other);
    
    struct jpeg_compress_struct cinfo;          /// libjpeg file structure
    struct jpeg_error_mgr jerr;                 /// libjpeg error structure

    std::string warningMsg;                     /// All the warnings.

    /// Destination file for the split-write path. Owned here rather than by a
    /// local in write(), because the handle now spans four public calls and
    /// error_exit() throws from inside libjpeg -- without an owner, any throw
    /// between beginWrite() and endWrite() would leak it with no enclosing
    /// scope to blame. Closed by endWrite(), by the destructor, and on error.
    FILE* outFile;
    bool  compressing;                          /// jpeg_start_compress is live
    bool  rowsStarted;                          /// first scanline written; markers now invalid

    void closeFile();                           /// close outFile if open; never throws
};

inline void JPEGWriter::closeFile() {
    if (outFile != NULL) { fclose(outFile); outFile = NULL; }
}

inline void JPEGWriter::setChromaSubsampling(const int mode) {
    // header() must have run: jpeg_set_defaults() is what allocates comp_info.
    assert(cinfo.comp_info != NULL &&
           "setChromaSubsampling() must be called AFTER header()");
    if (cinfo.comp_info == NULL || cinfo.num_components < 1)
        return;

    const int luma = (mode == 0) ? 1 : 2;       // 0 = 4:4:4, 1 = 4:2:0
    cinfo.comp_info[0].h_samp_factor = luma;
    cinfo.comp_info[0].v_samp_factor = luma;
    for (int i = 1; i < cinfo.num_components; ++i) {
        cinfo.comp_info[i].h_samp_factor = 1;
        cinfo.comp_info[i].v_samp_factor = 1;
    }
}

inline void JPEGWriter::setWriteJfifHeader(const bool enabled) {
    cinfo.write_JFIF_header = (boolean)enabled;
}

inline void JPEGWriter::beginWrite(const std::string& path) {
    assert(outFile == NULL && "beginWrite() called twice without endWrite()");
    closeFile();

    // SYS_FopenUtf8, not fopen: `path` is UTF-8 and fopen decodes it with the
    // process ANSI code page on Windows (see SYS_FileUtf8.h).
    if ((outFile = SYS_FopenUtf8(path.c_str(), "wb")) == NULL)
        throw std::runtime_error("Cannot open " + path);

    try {
        jpeg_stdio_dest(&cinfo, outFile);
        jpeg_start_compress(&cinfo, (boolean)true);
    } catch (...) {
        closeFile();
        throw;
    }
    compressing = true;
    rowsStarted = false;
}

inline void JPEGWriter::writeMarker(const int marker, const unsigned char* data,
                                    const unsigned len) {
    assert(compressing && "writeMarker() must follow beginWrite()");
    assert(!rowsStarted && "writeMarker() must precede the first writeRows()");

    // libjpeg caps one marker payload at 65533 (the 2-byte segment length
    // includes itself). Reject rather than chunk -- see the header comment.
    if (len > 65533u) {
        closeFile();
        compressing = false;
        throw std::runtime_error("JPEG marker payload too large for one segment");
    }

    try {
        jpeg_write_marker(&cinfo, marker, (const JOCTET*) data, len);
    } catch (...) {
        closeFile();
        compressing = false;
        throw;
    }
}

inline void JPEGWriter::endWrite() {
    if (!compressing) { closeFile(); return; }      // idempotent
    compressing = false;
    try {
        jpeg_finish_compress(&cinfo);
    } catch (...) {
        closeFile();
        throw;
    }
    closeFile();
}

inline void JPEGWriter::setQuality(const unsigned value, const bool forceBaseline) {
    assert(value >= 0 && value <= 100);
    jpeg_set_quality(&cinfo, value, (boolean)forceBaseline);
}

inline const std::string& JPEGWriter::warnings() const {
    return warningMsg;
}

template <typename RowPtrIter>
void JPEGWriter::writeRows(RowPtrIter rows) {
    assert(compressing && "writeRows() must follow beginWrite()");
    rowsStarted = true;

    try {
        // Bound strictly by the row count the caller actually supplied
        // (image_height), rather than by cinfo.next_scanline reaching
        // image_height. The two are supposed to march in lockstep --
        // jpeg_write_scanlines() is called with num_lines=1 each time -- but
        // relying on the library's own counter as the sole loop bound means
        // any discrepancy walks RowPtrIter past the end of the caller's
        // range with no way to detect it. Counting our own iterations here
        // makes that structurally impossible: the loop cannot run more than
        // image_height times regardless of what cinfo reports.
        for (JDIMENSION rowsWritten = 0; rowsWritten < cinfo.image_height; ++rowsWritten, ++rows) {
            unsigned char* row_ptr = *rows;
            jpeg_write_scanlines(&cinfo, &row_ptr, 1);
        }
    } catch (...) {
        closeFile();
        compressing = false;
        throw;
    }
}

/// Convenience wrapper over beginWrite/writeRows/endWrite for the common case
/// of a file with no APP markers.
template <typename RowPtrIter>
void JPEGWriter::write(const std::string& path, RowPtrIter rows) {
    beginWrite(path);
    writeRows(rows);
    endWrite();
}

template <typename RowPtrIter>
void JPEGWriter::write(unsigned char ** outbuffer, unsigned long * outsize, RowPtrIter rows)
{
	jpeg_mem_dest(&cinfo, outbuffer, outsize);

    jpeg_start_compress(&cinfo, (boolean)true);

    try {
        while (cinfo.next_scanline < cinfo.image_height) {
            unsigned char* row_ptr = *rows;
            jpeg_write_scanlines(&cinfo, &row_ptr, 1);
            ++rows;
        }

        jpeg_finish_compress(&cinfo);
    } catch (...) {
        // jpeg_mem_dest hands libjpeg a malloc'd buffer that the CALLER must
        // free and jpeg_destroy_compress does not. On a throw part-way through,
        // finish_compress never runs and *outbuffer is left indeterminate --
        // possibly allocated, possibly not, and unfreeable by anyone who cannot
        // tell which. Give the caller one unambiguous state instead.
        if (outbuffer != NULL && *outbuffer != NULL) {
            free(*outbuffer);
            *outbuffer = NULL;
        }
        if (outsize != NULL)
            *outsize = 0;
        throw;
    }
}


#endif
