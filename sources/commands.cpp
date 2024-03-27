#include "commands.h"
#include "btree_dir.h"
#include "crypto.h"
#include "exceptions.h"
#include "files.h"
#include "full_format.h"
#include "fuse_high_level_ops_base.h"
#include "git-version.h"
#include "lite_format.h"
#include "lock_enabled.h"
#include "logger.h"
#include "myutils.h"
#include "params.pb.h"
#include "params_io.h"
#include "platform.h"
#include "tags.h"
#include "win_get_proc.h"    // IWYU pragma: keep

#include <BS_thread_pool.hpp>
#include <absl/strings/escaping.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <argon2.h>
#include <cryptopp/cpu.h>
#include <cryptopp/hmac.h>
#include <cryptopp/osrng.h>
#include <cryptopp/scrypt.h>
#include <cryptopp/secblock.h>
#include <fruit/component.h>
#include <fruit/fruit.h>
#include <fruit/fruit_forward_decls.h>
#include <google/protobuf/util/json_util.h>
#include <string>
#include <tclap/CmdLine.h>
#include <tclap/SwitchArg.h>
#include <tclap/ValueArg.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <typeinfo>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

using namespace securefs;

namespace
{
constexpr std::string_view const kLegacyConfigFileName = ".securefs.json";
constexpr std::string_view const kConfigFileName = ".config.pb";
constexpr std::string_view EMPTY_PASSWORD_WHEN_KEY_FILE_IS_USED = " ";
}    // namespace

namespace securefs
{
/// A base class for all commands that require a data dir to be present.
class DataDirCommandBase : public CommandBase
{
protected:
    TCLAP::UnlabeledValueArg<std::string> data_dir{
        "dir", "Directory where the data are stored", true, "", "data_dir"};
    TCLAP::ValueArg<std::string> config_path{
        "",
        "config",
        "Full path name of the config file. ${data_dir}/.config.pb by default",
        false,
        "",
        "config_path"};

protected:
    std::string get_real_config_path_for_reading()
    {
        if (!config_path.getValue().empty())
        {
            return config_path.getValue();
        }
        OSService root(data_dir.getValue());
        fuse_stat st{};
        if (root.stat(std::string(kConfigFileName), &st))
        {
            return root.norm_path_narrowed(kConfigFileName);
        }
        if (root.stat(std::string(kLegacyConfigFileName), &st))
        {
            return root.norm_path_narrowed(kLegacyConfigFileName);
        }
        throw_runtime_error("No params file found. Please verify if the data dir is correct, or if "
                            "you should manually specify the params file.");
    }
};

static void secure_wipe(const char* buffer, size_t size)
{
    // On FreeBSD, this may be called. It is a no-op because we cannot write const region.
}

static void secure_wipe(char* buffer, size_t size)
{
    CryptoPP::SecureWipeBuffer(reinterpret_cast<byte*>(buffer), size);
}

void CommandBase::parse_cmdline(int argc, const char* const* argv) { cmdline()->parse(argc, argv); }

class SinglePasswordCommandBase : public DataDirCommandBase
{
protected:
    TCLAP::ValueArg<std::string> pass{
        "",
        "pass",
        "Password (prefer manually typing or piping since those methods are more secure)",
        false,
        "",
        "password"};
    TCLAP::ValueArg<std::string> keyfile{
        "",
        "keyfile",
        "An optional path to a key file to use in addition to or in place of password",
        false,
        "",
        "path"};
    TCLAP::SwitchArg askpass{
        "",
        "askpass",
        "When set to true, ask for password even if a key file is used. "
        "password+keyfile provides even stronger security than one of them alone.",
        false};
    CryptoPP::AlignedSecByteBlock password;

    void get_password(bool require_confirmation)
    {
        if (pass.isSet() && !pass.getValue().empty())
        {
            password.Assign(reinterpret_cast<const byte*>(pass.getValue().data()),
                            pass.getValue().size());
            secure_wipe(&pass.getValue()[0], pass.getValue().size());
            return;
        }
        if (keyfile.isSet() && !keyfile.getValue().empty() && !askpass.getValue())
        {
            password.Assign(
                reinterpret_cast<const byte*>(EMPTY_PASSWORD_WHEN_KEY_FILE_IS_USED.data()),
                EMPTY_PASSWORD_WHEN_KEY_FILE_IS_USED.size());
            return;
        }
        if (require_confirmation)
        {
            return OSService::read_password_with_confirmation("Enter password:", &password);
        }
        return OSService::read_password_no_confirmation("Enter password:", &password);
    }
    void add_all_args_from_base(TCLAP::CmdLine& cmd_line)
    {
        cmd_line.add(&data_dir);
        cmd_line.add(&config_path);
        cmd_line.add(&pass);
        cmd_line.add(&keyfile);
        cmd_line.add(&askpass);
    }
};

struct Argon2idArgsHolder
{
    TCLAP::ValueArg<unsigned> t{
        "", "argon2-t", "The time cost for argon2 algorithm", false, 30, "integer"};
    TCLAP::ValueArg<unsigned> m{"",
                                "argon2-m",
                                "The memory cost for argon2 algorithm (in terms of KiB)",
                                false,
                                1 << 18,
                                "integer"};
    TCLAP::ValueArg<unsigned> p{
        "", "argon2-p", "The parallelism for argon2 algorithm", false, 4, "integer"};

    void add_to(TCLAP::CmdLine& cmdline)
    {
        cmdline.add(&t);
        cmdline.add(&m);
        cmdline.add(&p);
    }

    EncryptedSecurefsParams::Argon2idParams to_params()
    {
        EncryptedSecurefsParams::Argon2idParams result;
        result.set_time_cost(t.getValue());
        result.set_memory_cost(m.getValue());
        result.set_parallelism(p.getValue());
        return result;
    }
};

class CreateCommand : public SinglePasswordCommandBase
{
private:
    TCLAP::ValueArg<std::string> format{
        "f",
        "format",
        "The format type of the repository. Either lite or full. Lite repos are faster and more "
        "reliable, but the directory structure itself is visible. Full repos offer more privacy at "
        "the cost of performance and ease of synchronization.",
        false,
        "lite",
        "lite/full"};
    TCLAP::ValueArg<unsigned int> iv_size{
        "", "iv-size", "The IV size (ignored for fs format 1)", false, 12, "integer"};
    TCLAP::ValueArg<unsigned int> block_size{
        "", "block-size", "Block size for files (ignored for fs format 1)", false, 4096, "integer"};
    TCLAP::ValueArg<unsigned> max_padding{
        "",
        "max-padding",
        "Maximum number of padding (the unit is byte) to add to all files in order to obfuscate "
        "their sizes. Each "
        "file has a different padding. Enabling this has a large performance cost.",
        false,
        0,
        "int"};
    TCLAP::ValueArg<unsigned int> long_name_threshold{
        "",
        "long-name-threshold",
        "(For lite format only) when the filename component exceeds this length, it will be stored "
        "encrypted in a SQLite database.",
        false,
        128,
        "integer"};
    TCLAP::ValueArg<std::string> case_handling{
        "",
        "case",
        "Either sensitive or insensitive. Changes how full format stores its filenames. Not "
        "applicable to lite format.",
        false,
        "insensitive",
        "sensitive/insensitive"};
    TCLAP::ValueArg<std::string> uninorm{
        "",
        "uninorm",
        "Either sensitive or insensitive. Changes how full format stores its filenames. Not "
        "applicable to lite format.",
        false,
        "insensitive",
        "sensitive/insensitive"};
    Argon2idArgsHolder argon2;

private:
    static void randomize(std::string* str, size_t size)
    {
        str->resize(size);
        generate_random(str->data(), str->size());
    }

public:
    std::unique_ptr<TCLAP::CmdLine> cmdline() override
    {
        auto result = make_unique<TCLAP::CmdLine>(help_message());
        add_all_args_from_base(*result);
        result->add(&iv_size);
        result->add(&block_size);
        result->add(&max_padding);
        result->add(&format);
        result->add(&case_handling);
        result->add(&uninorm);
        argon2.add_to(*result);
        return result;
    }

    void parse_cmdline(int argc, const char* const* argv) override
    {
        SinglePasswordCommandBase::parse_cmdline(argc, argv);
        get_password(true);
    }

    int execute() override
    {
        OSService::get_default().ensure_directory(data_dir.getValue(), 0755);

        DecryptedSecurefsParams params;
        params.mutable_size_params()->set_iv_size(iv_size.getValue());
        params.mutable_size_params()->set_block_size(block_size.getValue());
        params.mutable_size_params()->set_max_padding_size(max_padding.getValue());

        if (absl::EqualsIgnoreCase(format.getValue(), "lite") || format.getValue() == "4")
        {
            randomize(params.mutable_lite_format_params()->mutable_name_key(), 32);
            randomize(params.mutable_lite_format_params()->mutable_content_key(), 32);
            randomize(params.mutable_lite_format_params()->mutable_xattr_key(), 32);
            randomize(params.mutable_lite_format_params()->mutable_padding_key(), 32);

            if (long_name_threshold.getValue() > 0)
            {
                params.mutable_lite_format_params()->set_long_name_threshold(
                    long_name_threshold.getValue());
            }
        }
        else if (absl::EqualsIgnoreCase(format.getValue(), "full") || format.getValue() == "2")
        {
            randomize(params.mutable_full_format_params()->mutable_master_key(), 32);
            if (case_handling.getValue() == "insensitive")
            {
                params.mutable_full_format_params()->set_case_insensitive(true);
            }
            else if (case_handling.getValue() != "sensitive")
            {
                throw_runtime_error("Invalid value for --case: " + case_handling.getValue());
            }
            else if (is_windows())
            {
                WARN_LOG("It is recommended to add --case insensitive on Windows for full format "
                         "in order to match the default behavior of NTFS.");
            }
            if (uninorm.getValue() == "insensitive")
            {
                params.mutable_full_format_params()->set_unicode_normalization_agnostic(true);
            }
            else if (uninorm.getValue() != "sensitive")
            {
                throw_runtime_error("Invalid value for --uninorm: " + uninorm.getValue());
            }
            else if (is_apple())
            {
                WARN_LOG("It is recommended to add --uninorm insensitive on Apple for full format "
                         "in order to match the default behavior of APFS/HFS+.");
            }
        }
        else
        {
            throw_runtime_error("--format lite/full must be specified");
        }

        auto encrypted_data = encrypt(params,
                                      argon2.to_params(),
                                      {password.data(), password.size()},
                                      maybe_open_key_stream(keyfile.getValue()).get())
                                  .SerializeAsString();
        auto config_stream = OSService::get_default().open_file_stream(
            config_path.getValue().empty() ? absl::StrCat(data_dir.getValue(), "/", kConfigFileName)
                                           : config_path.getValue(),
            O_WRONLY | O_EXCL | O_CREAT,
            0644);
        config_stream->write(encrypted_data.data(), 0, encrypted_data.size());
        return 0;
    }

    const char* long_name() const noexcept override { return "create"; }

    char short_name() const noexcept override { return 'c'; }

    const char* help_message() const noexcept override { return "Create a new filesystem"; }
};

class ChangePasswordCommand : public DataDirCommandBase
{
private:
    CryptoPP::AlignedSecByteBlock old_password, new_password;
    TCLAP::ValueArg<std::string> old_key_file{
        "", "oldkeyfile", "Path to original key file", false, "", "path"};
    TCLAP::ValueArg<std::string> new_key_file{
        "", "newkeyfile", "Path to new key file", false, "", "path"};
    TCLAP::SwitchArg askoldpass{
        "",
        "askoldpass",
        "When set to true, ask for password even if a key file is used. "
        "password+keyfile provides even stronger security than one of them alone.",
        false};
    TCLAP::SwitchArg asknewpass{
        "",
        "asknewpass",
        "When set to true, ask for password even if a key file is used. "
        "password+keyfile provides even stronger security than one of them alone.",
        false};
    TCLAP::ValueArg<std::string> oldpass{
        "",
        "oldpass",
        "The old password (prefer manually typing or piping since those methods are more secure)",
        false,
        "",
        "string"};
    TCLAP::ValueArg<std::string> newpass{
        "",
        "newpass",
        "The new password (prefer manually typing or piping since those methods are more secure)",
        false,
        "",
        "string"};
    Argon2idArgsHolder argon2;

    static void assign(std::string_view value, CryptoPP::AlignedSecByteBlock& output)
    {
        output.Assign(reinterpret_cast<const byte*>(value.data()), value.size());
    }

public:
    std::unique_ptr<TCLAP::CmdLine> cmdline() override
    {
        auto result = make_unique<TCLAP::CmdLine>(help_message());
        result->add(&data_dir);
        result->add(&config_path);
        result->add(&old_key_file);
        result->add(&new_key_file);
        result->add(&askoldpass);
        result->add(&asknewpass);
        result->add(&oldpass);
        result->add(&newpass);
        argon2.add_to(*result);
        return result;
    }

    void parse_cmdline(int argc, const char* const* argv) override
    {
        DataDirCommandBase::parse_cmdline(argc, argv);

        if (oldpass.isSet())
        {
            assign(oldpass.getValue(), old_password);
        }
        else if (old_key_file.getValue().empty() || askoldpass.getValue())
        {
            OSService::read_password_no_confirmation("Old password: ", &old_password);
        }
        else
        {
            assign(EMPTY_PASSWORD_WHEN_KEY_FILE_IS_USED, old_password);
        }

        if (newpass.isSet())
        {
            assign(newpass.getValue(), new_password);
        }
        else if (new_key_file.getValue().empty() || asknewpass.getValue())
        {
            OSService::read_password_with_confirmation("New password: ", &new_password);
        }
        else
        {
            assign(EMPTY_PASSWORD_WHEN_KEY_FILE_IS_USED, new_password);
        }
    }

    int execute() override
    {
        auto original_path = get_real_config_path_for_reading();
        byte buffer[16];
        generate_random(buffer, array_length(buffer));
        auto tmp_path = original_path + hexify(buffer, array_length(buffer));
        auto stream = OSService::get_default().open_file_stream(original_path, O_RDONLY, 0644);
        auto params = decrypt(
            OSService::get_default().open_file_stream(original_path, O_RDONLY, 0644)->as_string(),
            {old_password.data(), old_password.size()},
            maybe_open_key_stream(old_key_file).get());
        auto encrypted_data = encrypt(params,
                                      argon2.to_params(),
                                      {new_password.data(), new_password.size()},
                                      maybe_open_key_stream(new_key_file).get())
                                  .SerializeAsString();
        stream = OSService::get_default().open_file_stream(
            tmp_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        DEFER(if (has_uncaught_exceptions()) {
            OSService::get_default().remove_file_nothrow(tmp_path);
        });
        stream->write(encrypted_data.data(), 0, encrypted_data.size());
        stream.reset();
        OSService::get_default().rename(tmp_path, original_path);
        return 0;
    }

    const char* long_name() const noexcept override { return "chpass"; }

    char short_name() const noexcept override { return 0; }

    const char* help_message() const noexcept override
    {
        return "Change password/keyfile of existing filesystem";
    }
};

class MountCommand : public SinglePasswordCommandBase
{
private:
    TCLAP::SwitchArg single_threaded{"s", "single", "Single threaded mode"};
    TCLAP::SwitchArg background{
        "b", "background", "Run securefs in the background (currently no effect on Windows)"};
    TCLAP::SwitchArg insecure{
        "i", "insecure", "Disable all integrity verification (insecure mode)"};
    TCLAP::SwitchArg noxattr{"x", "noxattr", "Disable built-in xattr support"};
    TCLAP::SwitchArg verbose{"v", "verbose", "Logs more verbose messages"};
    TCLAP::SwitchArg trace{"", "trace", "Trace all calls into `securefs` (implies --verbose)"};
    TCLAP::ValueArg<std::string> log{
        "", "log", "Path of the log file (may contain sensitive information)", false, "", "path"};
    TCLAP::MultiArg<std::string> fuse_options{
        "o",
        "opt",
        "Additional FUSE options; this may crash the filesystem; use only for testing!",
        false,
        "options"};
    TCLAP::UnlabeledValueArg<std::string> mount_point{
        "mount_point", "Mount point", true, "", "mount_point"};
    TCLAP::ValueArg<std::string> fsname{
        "", "fsname", "Filesystem name shown when mounted", false, "securefs", "fsname"};
    TCLAP::ValueArg<std::string> fssubtype{
        "", "fssubtype", "Filesystem subtype shown when mounted", false, "securefs", "fssubtype"};
    TCLAP::SwitchArg noflock{"",
                             "noflock",
                             "Disables the usage of file locking. Needed on some network "
                             "filesystems. May cause data loss, so use it at your own risk!",
                             false};
    TCLAP::ValueArg<std::string> normalization{"",
                                               "normalization",
                                               "Mode of filename normalization. Valid values: "
                                               "none, casefold, nfc, casefold+nfc. Defaults to nfc "
                                               "on macOS and none on other platforms",
                                               false,
#ifdef __APPLE__
                                               "nfc",
#else
                                               "none",
#endif
                                               ""};
    TCLAP::ValueArg<int> attr_timeout{"",
                                      "attr-timeout",
                                      "Number of seconds to cache file attributes. Default is 30.",
                                      false,
                                      30,
                                      "int"};
    TCLAP::SwitchArg skip_dot_dot{"",
                                  "skip-dot-dot",
                                  "When enabled, securefs will not return . and .. in `readdir` "
                                  "calls. You should normally not need this.",
                                  false};
    TCLAP::SwitchArg plain_text_names{"",
                                      "plain-text-names",
                                      "When enabled, securefs does not encrypt or decrypt file "
                                      "names. Use it at your own risk. No effect on full format.",
                                      false};

    DecryptedSecurefsParams fsparams{};

private:
    std::vector<const char*> to_c_style_args(const std::vector<std::string>& args)
    {
        std::vector<const char*> result(args.size());
        std::transform(args.begin(),
                       args.end(),
                       result.begin(),
                       [](const std::string& s) { return s.c_str(); });
        return result;
    }
#ifdef _WIN32
    static bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    static bool is_drive_mount(std::string_view mount_point)
    {
        return mount_point.size() == 2 && is_letter(mount_point[0]) && mount_point[1] == ':';
    }
    static bool is_network_mount(std::string_view mount_point)
    {
        return absl::StartsWith(mount_point, "\\\\") && !absl::StartsWith(mount_point, "\\\\?\\");
    }
#endif

    static std::string escape_args(const std::vector<std::string>& args)
    {
        std::string result;
        for (const auto& a : args)
        {
            result.push_back('\"');
            result.append(absl::Utf8SafeCEscape(a));
            result.push_back('\"');
            result.push_back(' ');
        }
        if (!result.empty())
        {
            result.pop_back();
        }
        return result;
    }

    static key_type from_byte_string(std::string_view view)
    {
        return key_type{reinterpret_cast<const byte*>(view.data()), view.size()};
    }

    static fruit::Component<FuseHighLevelOpsBase>
    get_fuse_high_ops_component(const MountCommand* cmd)
    {
        auto internal_binder = [](DecryptedSecurefsParams::FormatSpecificParamsCase format_case)
            -> fruit::Component<
                fruit::Required<lite_format::FuseHighLevelOps, full_format::FuseHighLevelOps>,
                FuseHighLevelOpsBase>
        {
            switch (format_case)
            {
            case DecryptedSecurefsParams::kLiteFormatParams:
                return fruit::createComponent()
                    .bind<FuseHighLevelOpsBase, lite_format::FuseHighLevelOps>();
            case DecryptedSecurefsParams::kFullFormatParams:
                return fruit::createComponent()
                    .bind<FuseHighLevelOpsBase, full_format::FuseHighLevelOps>();
            default:
                throwInvalidArgumentException("Unknown format case");
            }
        };

        return fruit::createComponent()
            .bindInstance(*cmd)
            .install(+internal_binder, cmd->fsparams.format_specific_params_case())
            .install(::securefs::lite_format::get_name_translator_component)
            .install(full_format::get_table_io_component,
                     cmd->fsparams.full_format_params().legacy_file_table_io())
            .registerProvider<lite_format::NameNormalizationFlags(const MountCommand&)>(
                [](const MountCommand& cmd)
                {
                    lite_format::NameNormalizationFlags flags{};
                    if (cmd.plain_text_names.getValue())
                    {
                        flags.no_op = true;
                    }
                    else if (cmd.normalization.getValue() == "nfc")
                    {
                        flags.should_normalize_nfc = true;
                    }
                    else if (cmd.normalization.getValue() == "casefold")
                    {
                        flags.should_case_fold = true;
                    }
                    else if (cmd.normalization.getValue() == "casefold+nfc")
                    {
                        flags.should_normalize_nfc = true;
                        flags.should_case_fold = true;
                    }
                    else if (cmd.normalization.getValue() != "none")
                    {
                        throw_runtime_error("Invalid flag of --normalization: "
                                            + cmd.normalization.getValue());
                    }
                    flags.long_name_threshold
                        = cmd.fsparams.lite_format_params().long_name_threshold();
                    return flags;
                })
            .registerProvider<fruit::Annotated<tSkipVerification, bool>(const MountCommand&)>(
                [](const MountCommand& cmd) { return cmd.insecure.getValue(); })
            .registerProvider<fruit::Annotated<tVerify, bool>(const MountCommand&)>(
                [](const MountCommand& cmd) { return !cmd.insecure.getValue(); })
            .registerProvider<fruit::Annotated<tStoreTimeWithinFs, bool>(const MountCommand&)>(
                [](const MountCommand& cmd)
                { return cmd.fsparams.full_format_params().store_time(); })
            .registerProvider<fruit::Annotated<tReadOnly, bool>(const MountCommand&)>(
                [](const MountCommand& cmd)
                {
                    // TODO: Support readonly mounts.
                    return false;
                })
            .registerProvider(
                []() { return new BS::thread_pool(std::thread::hardware_concurrency() * 2); })
            .bind<Directory, BtreeDirectory>()
            .registerProvider<fruit::Annotated<tMaxPaddingSize, unsigned>(const MountCommand&)>(
                [](const MountCommand& cmd)
                { return cmd.fsparams.size_params().max_padding_size(); })
            .registerProvider<fruit::Annotated<tIvSize, unsigned>(const MountCommand&)>(
                [](const MountCommand& cmd) { return cmd.fsparams.size_params().iv_size(); })
            .registerProvider<fruit::Annotated<tBlockSize, unsigned>(const MountCommand&)>(
                [](const MountCommand& cmd) { return cmd.fsparams.size_params().block_size(); })
            .registerProvider<fruit::Annotated<tMasterKey, key_type>(const MountCommand&)>(
                [](const MountCommand& cmd)
                { return from_byte_string(cmd.fsparams.full_format_params().master_key()); })
            .registerProvider<fruit::Annotated<tNameMasterKey, key_type>(const MountCommand&)>(
                [](const MountCommand& cmd)
                { return from_byte_string(cmd.fsparams.lite_format_params().name_key()); })
            .registerProvider<fruit::Annotated<tContentMasterKey, key_type>(const MountCommand&)>(
                [](const MountCommand& cmd)
                { return from_byte_string(cmd.fsparams.lite_format_params().content_key()); })
            .registerProvider<fruit::Annotated<tXattrMasterKey, key_type>(const MountCommand&)>(
                [](const MountCommand& cmd)
                { return from_byte_string(cmd.fsparams.lite_format_params().xattr_key()); })
            .registerProvider<fruit::Annotated<tPaddingMasterKey, key_type>(const MountCommand&)>(
                [](const MountCommand& cmd)
                {
                    if (cmd.fsparams.size_params().max_padding_size() > 0
                        || !cmd.fsparams.lite_format_params().padding_key().empty())
                        return from_byte_string(cmd.fsparams.lite_format_params().padding_key());
                    return key_type();
                })
            .registerProvider([](const MountCommand& cmd)
                              { return new OSService(cmd.data_dir.getValue()); })
            .registerProvider(
                [](const MountCommand& cmd)
                {
                    const auto& p = cmd.fsparams.full_format_params();
                    if (p.case_insensitive() && p.unicode_normalization_agnostic())
                    {
                        return Directory::DirNameComparison{&case_uni_norm_insensitve_compare};
                    }
                    if (p.case_insensitive())
                    {
                        return Directory::DirNameComparison{&case_insensitive_compare};
                    }
                    if (p.unicode_normalization_agnostic())
                    {
                        return Directory::DirNameComparison{&uni_norm_insensitive_compare};
                    }
                    return Directory::DirNameComparison{&binary_compare};
                })
            .registerProvider<fruit::Annotated<tCaseInsensitive, bool>(const MountCommand&)>(
                [](const MountCommand& cmd)
                { return cmd.fsparams.full_format_params().case_insensitive(); });
    }

public:
    std::unique_ptr<TCLAP::CmdLine> cmdline() override
    {
        auto result = make_unique<TCLAP::CmdLine>(help_message());
        add_all_args_from_base(*result);
        result->add(&background);
        // result->add(&insecure);
        result->add(&verbose);
        result->add(&trace);
        result->add(&log);
        result->add(&mount_point);
        result->add(&fuse_options);
        result->add(&single_threaded);
        result->add(&normalization);
        result->add(&fsname);
        result->add(&fssubtype);
        result->add(&noflock);
        result->add(&attr_timeout);
        result->add(&skip_dot_dot);
        result->add(&plain_text_names);
#ifdef __APPLE__
        result->add(&noxattr);
#endif
        return result;
    }

    void parse_cmdline(int argc, const char* const* argv) override
    {
        SinglePasswordCommandBase::parse_cmdline(argc, argv);

        get_password(false);

        if (global_logger && verbose.getValue())
            global_logger->set_level(LoggingLevel::kLogVerbose);
        if (global_logger && trace.getValue())
            global_logger->set_level(LoggingLevel::kLogTrace);

        set_lock_enabled(!noflock.getValue());
        if (noflock.getValue() && !single_threaded.getValue())
        {
            WARN_LOG("Using --noflock without --single is highly dangerous");
        }
    }

    void recreate_logger()
    {
        if (log.isSet())
        {
            auto logger = Logger::create_file_logger(log.getValue());
            delete global_logger;
            global_logger = logger;
        }
        else if (background.getValue())
        {
            WARN_LOG("securefs is about to enter background without a log file. You "
                     "won't be able to inspect what goes wrong. You can remount with "
                     "option --log instead.");
            delete global_logger;
            global_logger = nullptr;
        }
        if (global_logger && verbose.getValue())
            global_logger->set_level(LoggingLevel::kLogVerbose);
        if (global_logger && trace.getValue())
            global_logger->set_level(LoggingLevel::kLogTrace);
    }

    int execute() override
    {
        recreate_logger();
        if (background.getValue())
        {
            OSService::enter_background();
        }

        if (data_dir.getValue() == mount_point.getValue())
        {
            WARN_LOG("Mounting a directory on itself may cause securefs to hang");
        }

#ifdef _WIN32
        bool network_mount = is_network_mount(mount_point.getValue());
#else
        try
        {
            OSService::get_default().mkdir(mount_point.getValue(), 0755);
        }
        catch (const std::exception& e)
        {
            VERBOSE_LOG("%s (ignore this error if mounting succeeds eventually)", e.what());
        }
#endif
        std::string config_content;
        try
        {
            config_content = OSService::get_default()
                                 .open_file_stream(get_real_config_path_for_reading(), O_RDONLY, 0)
                                 ->as_string();
        }
        catch (const ExceptionBase& e)
        {
            if (e.error_number() == ENOENT)
            {
                ERROR_LOG("Encounter exception %s", e.what());
                ERROR_LOG(
                    "Config file %s does not exist. Perhaps you forget to run `create` command "
                    "first?",
                    get_real_config_path_for_reading().c_str());
                return 19;
            }
            throw;
        }
        fsparams = decrypt(config_content,
                           {password.data(), password.size()},
                           maybe_open_key_stream(keyfile.getValue()).get());
        CryptoPP::SecureWipeBuffer(password.data(), password.size());

        try
        {
            int fd_limit = OSService::raise_fd_limit();
            VERBOSE_LOG("Raising the number of file descriptor limit to %d", fd_limit);
        }
        catch (const std::exception& e)
        {
            WARN_LOG("Failure to raise the maximum file descriptor limit (%s: %s)",
                     get_type_name(e).get(),
                     e.what());
        }

        std::vector<std::string> fuse_args{
            "securefs",
            "-o",
            "hard_remove",
            "-o",
            "fsname=" + fsname.getValue(),
            "-o",
            "subtype=" + fssubtype.getValue(),
            "-o",
            absl::StrFormat("entry_timeout=%d", attr_timeout.getValue()),
            "-o",
            absl::StrFormat("attr_timeout=%d", attr_timeout.getValue()),
            "-o",
            absl::StrFormat("negative_timeout=%d", attr_timeout.getValue()),
#ifndef _WIN32
            "-o",
            "atomic_o_trunc",
#endif
        };
        if (single_threaded.getValue())
        {
            fuse_args.emplace_back("-s");
        }
        else
        {
#ifdef _WIN32
            fuse_args.emplace_back("-o");
            fuse_args.emplace_back(
                absl::StrFormat("ThreadCount=%d", 2 * std::thread::hardware_concurrency()));
#endif
        }
        // Handling `daemon` ourselves, as FUSE's version interferes with our initialization.
        fuse_args.emplace_back("-f");

#ifdef __APPLE__
        const char* copyfile_disable = ::getenv("COPYFILE_DISABLE");
        if (copyfile_disable)
        {
            VERBOSE_LOG("Mounting without .DS_Store and other apple dot files because "
                        "environmental "
                        "variable COPYFILE_DISABLE is set to \"%s\"",
                        copyfile_disable);
            fuse_args.emplace_back("-o");
            fuse_args.emplace_back("noappledouble");
        }
#elif _WIN32
        fuse_args.emplace_back("-ouid=-1,gid=-1,umask=0");
        if (network_mount)
        {
            fuse_args.emplace_back("--VolumePrefix=" + mount_point.getValue().substr(1));
        }
        fuse_args.emplace_back("-o");
        fuse_args.emplace_back(
            absl::StrFormat("FileInfoTimeout=%d", attr_timeout.getValue() * 1000));
        fuse_args.emplace_back("-o");
        fuse_args.emplace_back(
            absl::StrFormat("DirInfoTimeout=%d", attr_timeout.getValue() * 1000));
        fuse_args.emplace_back("-o");
        fuse_args.emplace_back(absl::StrFormat("EaTimeout=%d", attr_timeout.getValue() * 1000));
        fuse_args.emplace_back("-o");
        fuse_args.emplace_back(
            absl::StrFormat("VolumeInfoTimeout=%d", attr_timeout.getValue() * 1000));
#else
        fuse_args.emplace_back("-o");
        fuse_args.emplace_back("big_writes");
#endif
        if (fuse_options.isSet())
        {
            for (const std::string& opt : fuse_options.getValue())
            {
                fuse_args.emplace_back("-o");
                fuse_args.emplace_back(opt);
            }
        }

#ifdef _WIN32
        if (!network_mount)
#endif
            fuse_args.emplace_back(mount_point.getValue());

        fruit::Injector<FuseHighLevelOpsBase> injector(get_fuse_high_ops_component, this);

        bool native_xattr = !noxattr.getValue();
#ifdef __APPLE__
        if (native_xattr)
        {
            auto rc = OSService::get_default().listxattr(data_dir.getValue().c_str(), nullptr, 0);
            if (rc < 0)
            {
                absl::FPrintF(stderr,
                              "Warning: the filesystem under %s has no extended attribute "
                              "support.\nXattr is disabled\n",
                              data_dir.getValue());
                native_xattr = false;
            }
        }
#endif
        auto high_level_ops = injector.get<FuseHighLevelOpsBase*>();
        auto fuse_callbacks = FuseHighLevelOpsBase::build_ops(high_level_ops, native_xattr);
        VERBOSE_LOG("Calling fuse_main with arguments: %s", escape_args(fuse_args));
        return fuse_main(static_cast<int>(fuse_args.size()),
                         const_cast<char**>(to_c_style_args(fuse_args).data()),
                         &fuse_callbacks,
                         high_level_ops);
    }

    const char* long_name() const noexcept override { return "mount"; }

    char short_name() const noexcept override { return 'm'; }

    const char* help_message() const noexcept override { return "Mount an existing filesystem"; }
};

class VersionCommand : public CommandBase
{
public:
    std::unique_ptr<TCLAP::CmdLine> cmdline() override
    {
        auto result = make_unique<TCLAP::CmdLine>(help_message());
        return result;
    }

    int execute() override
    {
        using namespace CryptoPP;
        absl::PrintF("securefs %s\n", GIT_VERSION);
        absl::PrintF("Crypto++ %g\n", CRYPTOPP_VERSION / 100.0);
#ifdef _WIN32
        HMODULE hd = GetModuleHandleW((sizeof(void*) == 8) ? L"winfsp-x64.dll" : L"winfsp-x86.dll");
        NTSTATUS(*fsp_version_func)
        (uint32_t*) = get_proc_address<decltype(fsp_version_func)>(hd, "FspVersion");
        if (fsp_version_func)
        {
            uint32_t vn;
            if (fsp_version_func(&vn) == 0)
            {
                absl::PrintF("WinFsp %u.%u\n", vn >> 16, vn & 0xFFFFu);
            }
        }
#else
        typedef int version_function(void);
        auto fuse_version_func
            = reinterpret_cast<version_function*>(::dlsym(RTLD_DEFAULT, "fuse_version"));
        absl::PrintF("libfuse %d\n", fuse_version_func());
#endif

#ifdef CRYPTOPP_DISABLE_ASM
        fputs("\nBuilt without hardware acceleration\n", stdout);
#else
#if CRYPTOPP_BOOL_X86 || CRYPTOPP_BOOL_X32 || CRYPTOPP_BOOL_X64
        absl::PrintF("\nHardware features available:\nSSE2: %s\nSSE3: %s\nSSE4.1: %s\nSSE4.2: "
                     "%s\nAES-NI: %s\nCLMUL: %s\nSHA: %s\n",
                     HasSSE2() ? "true" : "false",
                     HasSSSE3() ? "true" : "false",
                     HasSSE41() ? "true" : "false",
                     HasSSE42() ? "true" : "false",
                     HasAESNI() ? "true" : "false",
                     HasCLMUL() ? "true" : "false",
                     HasSHA() ? "true" : "false");
#endif
#endif
        return 0;
    }

    const char* long_name() const noexcept override { return "version"; }

    char short_name() const noexcept override { return 'v'; }

    const char* help_message() const noexcept override { return "Show version of the program"; }
};

class InfoCommand : public SinglePasswordCommandBase
{
public:
    std::unique_ptr<TCLAP::CmdLine> cmdline() override
    {
        auto result = make_unique<TCLAP::CmdLine>(help_message());
        add_all_args_from_base(*result);
        return result;
    }

    const char* long_name() const noexcept override { return "info"; }

    char short_name() const noexcept override { return 'i'; }

    const char* help_message() const noexcept override
    {
        return "Display information about the filesystem";
    }

    void parse_cmdline(int argc, const char* const* argv) override
    {
        SinglePasswordCommandBase::parse_cmdline(argc, argv);
        get_password(false);
    }

    int execute() override
    {
        auto real_config_path = get_real_config_path_for_reading();
        auto params = decrypt(OSService::get_default()
                                  .open_file_stream(get_real_config_path_for_reading(), O_RDONLY, 0)
                                  ->as_string(),
                              {password.data(), password.size()},
                              maybe_open_key_stream(keyfile.getValue()).get());
        std::string json;
        google::protobuf::util::JsonPrintOptions options{};
        options.preserve_proto_field_names = true;
        options.add_whitespace = true;
        options.always_print_primitive_fields = true;
        auto status = google::protobuf::util::MessageToJsonString(params, &json, options);
        if (!status.ok())
        {
            throw_runtime_error("Failed to convert params to JSON: " + status.ToString());
        }

        absl::PrintF("Config file path: %s\n", real_config_path);
        absl::PrintF("JSON representation of config:\n%s\n", json);
        return 0;
    }
};

class DocCommand : public CommandBase
{
private:
    std::vector<CommandBase*> commands{};

public:
    DocCommand() = default;
    ~DocCommand() = default;
    const char* long_name() const noexcept override { return "doc"; }
    char short_name() const noexcept override { return 0; }
    const char* help_message() const noexcept override
    {
        return "Display the full help message of all commands in markdown format";
    }
    std::unique_ptr<TCLAP::CmdLine> cmdline() override
    {
        return make_unique<TCLAP::CmdLine>(help_message());
    }
    void add_command(CommandBase* c) { commands.push_back(c); }
    int execute() override
    {
        fputs("# securefs\n", stdout);
        fputs("The command strucuture is `securefs ${SUBCOMMAND} ${SUBOPTIONS}`.\nSee below for "
              "available subcommands and relevant options\n\n",
              stdout);
        for (auto c : commands)
        {
            if (c->short_name())
                absl::PrintF("## %s (short name: %c)\n", c->long_name(), c->short_name());
            else
                absl::PrintF("## %s\n", c->long_name());
            absl::PrintF("%s\n\n", c->help_message());
            auto cmdline = c->cmdline();

            std::vector<std::pair<size_t, TCLAP::Arg*>> prioritizedArgs;
            size_t index = 0;
            for (TCLAP::Arg* arg : cmdline->getArgList())
            {
                ++index;
                if (dynamic_cast<TCLAP::UnlabeledValueArg<std::string>*>(arg))
                {
                    prioritizedArgs.emplace_back(index, arg);
                }
                else
                {
                    prioritizedArgs.emplace_back(2 * cmdline->getArgList().size() - index, arg);
                }
            }
            std::sort(prioritizedArgs.begin(), prioritizedArgs.end());

            for (auto&& pair : prioritizedArgs)
            {
                TCLAP::Arg* arg = pair.second;
                {
                    auto a = dynamic_cast<TCLAP::UnlabeledValueArg<std::string>*>(arg);
                    if (a)
                    {
                        absl::PrintF(
                            "- **%s**: (*positional*) %s\n", a->getName(), a->getDescription());
                        continue;
                    }
                }
                if (arg->getName() == "ignore_rest" || arg->getName() == "version"
                    || arg->getName() == "help")
                {
                    continue;
                }
                fputs("- ", stdout);
                auto flag = arg->getFlag();
                if (!flag.empty())
                {
                    absl::PrintF("**%c%s** or ", arg->flagStartChar(), flag);
                }
                absl::PrintF("**%s%s**", arg->nameStartString(), arg->getName());
                absl::PrintF(": %s. ", arg->getDescription());
                {
                    auto a = dynamic_cast<TCLAP::SwitchArg*>(arg);
                    if (a)
                    {
                        if (a->getValue())
                        {
                            fputs("*This is a switch arg. Default: true.*\n", stdout);
                        }
                        else
                        {
                            fputs("*This is a switch arg. Default: false.*\n", stdout);
                        }
                        continue;
                    }
                }
                {
                    auto a = dynamic_cast<TCLAP::ValueArg<std::string>*>(arg);
                    if (a)
                    {
                        if (a->getValue().empty())
                        {
                            fputs("*Unset by default.*\n", stdout);
                        }
                        else
                        {
                            absl::PrintF("*Default: %s.*\n", a->getValue());
                        }
                        continue;
                    }
                }
                {
                    auto a = dynamic_cast<TCLAP::ValueArg<unsigned>*>(arg);
                    if (a)
                    {
                        absl::PrintF("*Default: %u.*\n", a->getValue());
                        continue;
                    }
                }
                {
                    auto a = dynamic_cast<TCLAP::ValueArg<int>*>(arg);
                    if (a)
                    {
                        absl::PrintF("*Default: %d.*\n", a->getValue());
                        continue;
                    }
                }
                {
                    auto a = dynamic_cast<TCLAP::MultiArg<std::string>*>(arg);
                    if (a)
                    {
                        fputs("*This option can be specified multiple times.*\n", stdout);
                        continue;
                    }
                }
                throw_runtime_error(std::string("Unknown type of arg ") + typeid(*arg).name());
            }
        }
        return 0;
    }
};

int commands_main(int argc, const char* const* argv)
{
    try
    {
        std::ios_base::sync_with_stdio(false);
        std::unique_ptr<CommandBase> cmds[] = {make_unique<MountCommand>(),
                                               make_unique<CreateCommand>(),
                                               make_unique<ChangePasswordCommand>(),
                                               make_unique<VersionCommand>(),
                                               make_unique<InfoCommand>(),
                                               make_unique<DocCommand>()};

        const char* const program_name = argv[0];
        auto doc_command = dynamic_cast<DocCommand*>(cmds[array_length(cmds) - 1].get());
        for (auto&& c : cmds)
        {
            doc_command->add_command(c.get());
        }

        auto print_usage = [&]()
        {
            fputs("Available subcommands:\n\n", stderr);

            for (auto&& command : cmds)
            {
                if (command->short_name())
                {
                    absl::FPrintF(stderr,
                                  "%s (alias: %c): %s\n",
                                  command->long_name(),
                                  command->short_name(),
                                  command->help_message());
                }
                else
                {
                    absl::FPrintF(
                        stderr, "%s: %s\n", command->long_name(), command->help_message());
                }
            }

            absl::FPrintF(stderr, "\nType %s ${SUBCOMMAND} --help for details\n", program_name);
            return 1;
        };

        if (argc < 2)
            return print_usage();
        argc--;
        argv++;

        for (auto&& command : cmds)
        {
            if (strcmp(argv[0], command->long_name()) == 0
                || (argv[0] != nullptr && argv[0][0] == command->short_name() && argv[0][1] == 0))
            {
                command->parse_cmdline(argc, argv);
                return command->execute();
            }
        }
        return print_usage();
    }
    catch (const TCLAP::ArgException& e)
    {
        ERROR_LOG("Error parsing arguments: %s at %s\n", e.error(), e.argId());
        return 5;
    }
    catch (const std::runtime_error& e)
    {
        ERROR_LOG("%s\n", e.what());
        return 1;
    }
    catch (const std::exception& e)
    {
        ERROR_LOG("%s: %s\n", get_type_name(e).get(), e.what());
        return 2;
    }
}
}    // namespace securefs
