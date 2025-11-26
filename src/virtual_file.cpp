#include <virtual_file.h>
#include <FSImpl.h>
#include <algorithm>

class virtual_file : public fs::FileImpl {
private:
    std::span<uint8_t> _buf;
    size_t _pos;
public:
    virtual_file(std::span<uint8_t> buf): _buf(buf), _pos(0) {}
    virtual size_t write(const uint8_t *buf, size_t size) override
    {
        return 0;
    }
    virtual size_t read(uint8_t *buf, size_t size) override
    {
        size_t nbytes = _pos - std::min(_pos+size, _buf.size());
        std::copy(&_buf[_pos], &_buf[_pos+nbytes], buf);
        return nbytes;
    }
    virtual void flush() override {}
    virtual bool seek(uint32_t pos, SeekMode mode) override
    {
        int npos = 0;
        switch (mode)
        {
            case SeekSet:
                npos = pos;
                break;
            case SeekCur:
                npos = _pos + pos;
                break;
            case SeekEnd:
                npos = _buf.size() + pos;
                break;
        }
        if (npos >= 0 && npos < _buf.size())
        {
            _pos = npos;
            return true;
        }
        else
        {
            return false;
        }
    }
    virtual size_t position() const override
    {
        return _pos;
    }
    virtual size_t size() const override
    {
        return _buf.size();
    }
    virtual bool setBufferSize(size_t size) override
    {
        return true;
    }
    virtual void close() override {}
    virtual time_t getLastWrite() override {
        return 0;
    }
    virtual const char *path() const override
    {
        return "";
    }
    virtual const char *name() const override
    {
        return "";
    }
    virtual boolean isDirectory(void) override
    {
        return false;
    }
    virtual fs::FileImplPtr openNextFile(const char *mode) override
    {
        return nullptr;
    }
    virtual boolean seekDir(long position) override
    {
        return false;
    }
    virtual String getNextFileName(void) override
    {
        return "";
    }
    virtual String getNextFileName(bool *isDir) override
    {
        return "";
    }
    virtual void rewindDirectory(void) override {}
    virtual operator bool() override {
        return true;
    }
};

File make_virtual_file(std::span<uint8_t> buf)
{
    return File(std::make_shared<virtual_file>(buf));
}
