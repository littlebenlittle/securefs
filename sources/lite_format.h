#pragma once

#include "fuse_high_level_ops_base.h"
#include "lite_stream.h"
#include "lock_guard.h"
#include "platform.h"
#include "tags.h"
#include "thread_local.h"

#include <absl/types/optional.h>
#include <cryptopp/aes.h>
#include <fruit/fruit.h>

namespace securefs
{
namespace lite_format
{
    class StreamOpener : public lite::AESGCMCryptStream::ParamCalculator
    {
    public:
        INJECT(StreamOpener(ANNOTATED(tContentMasterKey, key_type) content_master_key,
                            ANNOTATED(tPaddingMasterKey, key_type) padding_master_key,
                            ANNOTATED(tBlockSize, unsigned) block_size,
                            ANNOTATED(tIvSize, unsigned) iv_size,
                            ANNOTATED(tMaxPaddingSize, unsigned) max_padding_size,
                            ANNOTATED(tSkipVerification, bool) skip_verfication))
            : content_master_key_(content_master_key)
            , padding_master_key_(padding_master_key)
            , block_size_(block_size)
            , iv_size_(iv_size)
            , max_padding_size_(max_padding_size)
            , skip_verification_(skip_verfication)
        {
        }

        std::unique_ptr<securefs::lite::AESGCMCryptStream> open(std::shared_ptr<StreamBase> base);

        virtual void compute_session_key(const std::array<unsigned char, 16>& id,
                                         std::array<unsigned char, 16>& outkey) override;
        virtual unsigned compute_padding(const std::array<unsigned char, 16>& id) override;

    private:
        using AES_ECB = CryptoPP::ECB_Mode<CryptoPP::AES>::Encryption;
        AES_ECB& get_thread_local_content_master_enc();
        AES_ECB& get_thread_local_padding_master_enc();

    private:
        key_type content_master_key_, padding_master_key_;
        unsigned block_size_, iv_size_, max_padding_size_;
        bool skip_verification_;
        ThreadLocal content_ecb, padding_ecb;
    };

    class File;
    class Directory;

    class Base : public Object
    {
    public:
        virtual File* as_file() noexcept { return nullptr; }
        virtual Directory* as_dir() noexcept { return nullptr; }
    };

    class ABSL_LOCKABLE Directory : public Base, public DirectoryTraverser
    {
    private:
        securefs::Mutex m_lock;

    public:
        void lock() ABSL_EXCLUSIVE_LOCK_FUNCTION() { m_lock.lock(); }
        void unlock() noexcept ABSL_UNLOCK_FUNCTION() { m_lock.unlock(); }
        Directory* as_dir() noexcept { return this; }

        // Obtains the (virtual) path of the directory.
        virtual absl::string_view path() const = 0;

        // Redeclare the methods in `DirectoryTraverser` to add thread safe annotations.
        virtual bool next(std::string* name, fuse_stat* st) ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this)
            = 0;
        virtual void rewind() ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this) = 0;
    };

    class ABSL_LOCKABLE File final : public Base
    {
    private:
        std::unique_ptr<lite::AESGCMCryptStream> m_crypt_stream ABSL_GUARDED_BY(*this);
        std::shared_ptr<securefs::FileStream> m_file_stream ABSL_GUARDED_BY(*this);
        securefs::Mutex m_lock;

    public:
        File(std::shared_ptr<securefs::FileStream> file_stream, StreamOpener& opener)
            : m_file_stream(file_stream)
        {
            LockGuard<FileStream> lock_guard(*m_file_stream, true);
            m_crypt_stream = opener.open(file_stream);
        }

        ~File();

        length_type size() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this)
        {
            return m_crypt_stream->size();
        }
        void flush() ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this) { m_crypt_stream->flush(); }
        bool is_sparse() const noexcept ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this)
        {
            return m_crypt_stream->is_sparse();
        }
        void resize(length_type len) ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this)
        {
            m_crypt_stream->resize(len);
        }
        length_type read(void* output, offset_type off, length_type len)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this)
        {
            return m_crypt_stream->read(output, off, len);
        }
        void write(const void* input, offset_type off, length_type len)
            ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this)
        {
            return m_crypt_stream->write(input, off, len);
        }
        void fstat(fuse_stat* stat) ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this);
        void fsync() ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this) { m_file_stream->fsync(); }
        void utimens(const fuse_timespec ts[2]) ABSL_EXCLUSIVE_LOCKS_REQUIRED(*this)
        {
            m_file_stream->utimens(ts);
        }
        void lock(bool exclusive = true) ABSL_EXCLUSIVE_LOCK_FUNCTION()
        {
            m_lock.lock();
            try
            {
                m_file_stream->lock(exclusive);
            }
            catch (...)
            {
                m_lock.unlock();
                throw;
            }
        }
        void unlock() noexcept ABSL_UNLOCK_FUNCTION()
        {
            m_file_stream->unlock();
            m_lock.unlock();
        }
        File* as_file() noexcept override { return this; }
    };

    struct NameTranslator : public Object
    {

        /// @brief Encrypt the full path.
        /// @param path The original path.
        /// @param out_encrypted_last_component If it is not null, and the last path component is a
        /// long component, then this contains the encrypted version of the last path component.
        /// @return Encrypted path.
        virtual std::string encrypt_full_path(absl::string_view path,
                                              std::string* out_encrypted_last_component)
            = 0;

        /// @brief Decrypt a component of an encrypted path.
        /// If a long component, then the result is empty.
        virtual std::string decrypt_path_component(absl::string_view path) = 0;

        virtual std::string encrypt_path_for_symlink(absl::string_view path) = 0;
        virtual std::string decrypt_path_from_symlink(absl::string_view path) = 0;

        virtual unsigned max_virtual_path_component_size(unsigned physical_path_component_size) = 0;

        static absl::string_view get_last_component(absl::string_view path);
        static absl::string_view remove_last_component(absl::string_view path);
    };

    struct NameNormalizationArgs
    {
        bool should_case_fold;
        bool should_normalize_nfc;
        bool supports_long_name;

        bool operator==(const NameNormalizationArgs& other) const noexcept
        {
            return should_case_fold == other.should_case_fold
                && should_normalize_nfc == other.should_normalize_nfc
                && supports_long_name == other.supports_long_name;
        }
    };

    fruit::Component<fruit::Required<fruit::Annotated<tNameMasterKey, key_type>>, NameTranslator>
    get_name_translator_component(NameNormalizationArgs args);

    class FuseHighLevelOps : public ::securefs::FuseHighLevelOpsBase
    {
    public:
        INJECT(FuseHighLevelOps(::securefs::OSService& root,
                                StreamOpener& opener,
                                NameTranslator& name_trans))
            : root_(root), opener_(opener), name_trans_(name_trans)
        {
        }

        virtual void initialize(fuse_conn_info* info) override;
        virtual int vstatfs(const char* path, fuse_statvfs* buf, const fuse_context* ctx) override;
        virtual int vgetattr(const char* path, fuse_stat* st, const fuse_context* ctx) override;
        virtual int vfgetattr(const char* path,
                              fuse_stat* st,
                              fuse_file_info* info,
                              const fuse_context* ctx) override;
        virtual int
        vopendir(const char* path, fuse_file_info* info, const fuse_context* ctx) override;
        virtual int
        vreleasedir(const char* path, fuse_file_info* info, const fuse_context* ctx) override;
        virtual int vreaddir(const char* path,
                             void* buf,
                             fuse_fill_dir_t filler,
                             fuse_off_t off,
                             fuse_file_info* info,
                             const fuse_context* ctx) override;
        virtual int vcreate(const char* path,
                            fuse_mode_t mode,
                            fuse_file_info* info,
                            const fuse_context* ctx) override;
        virtual int vopen(const char* path, fuse_file_info* info, const fuse_context* ctx) override;
        virtual int
        vrelease(const char* path, fuse_file_info* info, const fuse_context* ctx) override;
        virtual int vread(const char* path,
                          char* buf,
                          size_t size,
                          fuse_off_t offset,
                          fuse_file_info* info,
                          const fuse_context* ctx) override;
        virtual int vwrite(const char* path,
                           const char* buf,
                           size_t size,
                           fuse_off_t offset,
                           fuse_file_info* info,
                           const fuse_context* ctx) override;
        virtual int
        vflush(const char* path, fuse_file_info* info, const fuse_context* ctx) override;
        virtual int vftruncate(const char* path,
                               fuse_off_t len,
                               fuse_file_info* info,
                               const fuse_context* ctx) override;
        virtual int vunlink(const char* path, const fuse_context* ctx) override;
        virtual int vmkdir(const char* path, fuse_mode_t mode, const fuse_context* ctx) override;
        virtual int vrmdir(const char* path, const fuse_context* ctx) override;
        virtual int vchmod(const char* path, fuse_mode_t mode, const fuse_context* ctx) override;
        virtual int
        vchown(const char* path, fuse_uid_t uid, fuse_gid_t gid, const fuse_context* ctx) override;
        virtual int vsymlink(const char* to, const char* from, const fuse_context* ctx) override;
        virtual int vlink(const char* src, const char* dest, const fuse_context* ctx) override;
        virtual int
        vreadlink(const char* path, char* buf, size_t size, const fuse_context* ctx) override;
        virtual int vrename(const char* from, const char* to, const fuse_context* ctx) override;
        virtual int vfsync(const char* path,
                           int datasync,
                           fuse_file_info* info,
                           const fuse_context* ctx) override;
        virtual int vtruncate(const char* path, fuse_off_t len, const fuse_context* ctx) override;
        virtual int
        vutimens(const char* path, const fuse_timespec* ts, const fuse_context* ctx) override;
        virtual int
        vlistxattr(const char* path, char* list, size_t size, const fuse_context* ctx) override;
        virtual int vgetxattr(const char* path,
                              const char* name,
                              char* value,
                              size_t size,
                              uint32_t position,
                              const fuse_context* ctx) override;
        virtual int vsetxattr(const char* path,
                              const char* name,
                              const char* value,
                              size_t size,
                              int flags,
                              uint32_t position,
                              const fuse_context* ctx) override;
        virtual int
        vremovexattr(const char* path, const char* name, const fuse_context* ctx) override;

    private:
        ::securefs::OSService& root_;
        StreamOpener& opener_;
        NameTranslator& name_trans_;
    };

}    // namespace lite_format

}    // namespace securefs

template <>
struct std::hash<securefs::lite_format::NameNormalizationArgs>
{
    std::size_t operator()(const securefs::lite_format::NameNormalizationArgs& args) const noexcept
    {
        return args.should_case_fold + args.should_normalize_nfc + args.supports_long_name;
    }
};