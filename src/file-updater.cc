#include "update-client-internal.hpp"

#include "file-updater.h"

#include "logger/log.h"
#include <aclapi.h>

FileUpdater::FileUpdater(fs::path old_files_dir, fs::path app_dir, fs::path new_files_dir, const manifest_map_t &manifest,
			 const local_manifest_t &local_manifest, update_client *client)
	: m_new_files_dir(new_files_dir),
	  m_old_files_dir(old_files_dir),
	  m_app_dir(app_dir),
	  m_manifest(manifest),
	  m_local_manifest(local_manifest),
	  m_update_client(client)
{
	m_old_files_dir /= "old-files";
}

FileUpdater::~FileUpdater()
{
	std::error_code ec;

	fs::remove_all(m_old_files_dir, ec);
	if (ec) {
		wlog_warn(L"Failed to cleanup temp folder.");
	}
}

void FileUpdater::update()
{
	std::string version_file_key = "resources\app.asar";
	manifest_map_t::const_iterator iter = m_manifest.begin();

	while (iter != m_manifest.end()) {
		if (version_file_key.compare(iter->first) != 0) {
			update_entry_with_retries(iter, m_new_files_dir);
		}
		++iter;
	}

	manifest_map_t::const_iterator version_file = m_manifest.find(version_file_key);
	if (version_file != m_manifest.end()) {
		update_entry_with_retries(version_file, m_new_files_dir);
	}

	if (!is_local_files_updated()) {
		throw std::runtime_error("Error: Update went not as expected");
	}
}

void FileUpdater::update_entry_with_retries(manifest_map_t::const_iterator &iter, fs::path &new_files_dir)
{
	int retries = 0;
	const int max_retries = 5;
	bool is_updated = false;

	while (retries < max_retries) {
		std::error_code ret;
		retries++;
		ret = update_entry(iter, new_files_dir);
		if (ret == std::errc::no_space_on_device) {
			if (m_update_client->check_disk_space()) {
				retries = 1;
				continue;
			} else {
				std::wstring wmsg = ConvertToUtf16WS(iter->first);
				wlog_warn(L"Have failed to update file: %s, no space on device", wmsg.c_str());
				throw std::runtime_error("Error: no space on device");
			}
		} else if (ret) {
			if (retries == 1) {
				std::wstring wmsg = ConvertToUtf16WS(iter->first);
				wlog_warn(L"Have failed to update file: %s, will retry", wmsg.c_str());
			}
			Sleep(100 * retries);
			continue;
		} else {
			is_updated = true; 
			break;
		}
	}

	if (!is_updated) {
		std::wstring wmsg = ConvertToUtf16WS(iter->first);
		wlog_warn(L"Have failed to update file: %s", wmsg.c_str());
		throw std::runtime_error("Error: failed to update file");
	}
}

std::error_code FileUpdater::update_entry(manifest_map_t::const_iterator &iter, fs::path &new_files_dir)
{
	std::error_code ec;

	if (iter->second.skip_update || iter->second.remove_at_update)
		return ec;

	try {
		fs::path file_name_part = fs::u8path(iter->first.c_str());
		fs::path to_path(m_app_dir);
		to_path /= file_name_part;

		fs::path old_file_path(m_old_files_dir);
		old_file_path /= file_name_part;

		fs::path from_path(new_files_dir);
		from_path /= file_name_part;

		fs::create_directories(to_path.parent_path(), ec);
		if (ec) {
			std::wstring wmsg = ConvertToUtf16WS(ec.message());
			wlog_warn(L"Failed to create directory: %s error, %s", to_path.parent_path().c_str(), wmsg.c_str());
		} else {
			fs::rename(from_path, to_path, ec);
			if (ec) {
				std::wstring wmsg = ConvertToUtf16WS(ec.message());
				wlog_debug(L"Failed to move file %s %s, error %s", from_path.c_str(), to_path.c_str(), wmsg.c_str());
			} else {
				try {
					reset_rights(to_path);
				} catch (...) {
					wlog_warn(L"Have failed to update file rights: %s", to_path.c_str());
				}
			}
		}
	} catch (...) {
		ec = std::make_error_code(std::errc::io_error);
	}

	return ec;
}

bool FileUpdater::reset_rights(const fs::path &path)
{
	ACL empty_acl;
	if (InitializeAcl(&empty_acl, sizeof(empty_acl), ACL_REVISION)) {
		const std::wstring path_str = path.generic_wstring();
		DWORD result = SetNamedSecurityInfo((LPWSTR)path_str.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
						    0, 0, &empty_acl, 0);
		if (result == ERROR_SUCCESS) {
			return true;
		}
	}
	return false;
}

void FileUpdater::revert()
{
	/* Generate the manifest for the current application directory */
	fs::recursive_directory_iterator iter(m_old_files_dir);
	fs::recursive_directory_iterator end_iter{};
	std::error_code ec;
	int error_count = 0;

	for (; iter != end_iter; ++iter) {
		if (fs::is_directory(iter->path())) {
			continue;
		}
		/* Fetch relative paths */
		fs::path rel_path(fs::relative(iter->path(), m_old_files_dir));

		fs::path to_path(m_app_dir);
		to_path /= rel_path;

		fs::remove(to_path, ec);
		if (ec) {
			wlog_warn(L"Revert have failed to correctly remove changed file: %s ", to_path.c_str());
			error_count++;
		}

		fs::rename(iter->path(), to_path, ec);
		if (ec) {
			wlog_warn(L"Revert have failed to correctly move file back: %s ", to_path.c_str());
			error_count++;
		}
	}

	if (error_count > 0 || is_local_files_changed()) {
		wlog_warn(L"Revert have failed to correctly revert some files. Fails: %i", error_count);
		throw std::exception("Revert have failed to correctly revert some files");
	}
}

bool FileUpdater::backup()
{
	manifest_map_t::const_iterator iter = m_manifest.begin();
	while (iter != m_manifest.end()) {
		if (iter->second.skip_update || !iter->second.compared_to_local) {
			iter++;
			continue;
		}

		try {
			std::error_code ec;
			fs::path file_name_part = fs::u8path(iter->first.c_str());
			fs::path to_path(m_app_dir);
			to_path /= file_name_part;

			fs::path old_file_path(m_old_files_dir);
			old_file_path /= file_name_part;

			fs::create_directories(old_file_path.parent_path(), ec);
			fs::create_directories(to_path.parent_path(), ec);

			if (fs::exists(to_path, ec)) {
				fs::rename(to_path, old_file_path, ec);
				if (ec == std::errc::no_space_on_device) {
					if (m_update_client->check_disk_space()) {
						continue;
					} else {
						wlog_error(L"Failed to backup entry %s to %s, error %s", to_path.c_str(), old_file_path.c_str(),
							   ec.message().c_str());
						return false;
					}
				} else if (ec) {
					std::wstring wmsg = ConvertToUtf16WS(ec.message());

					wlog_debug(L"Failed to backup entry %s to %s, error %s", to_path.c_str(), old_file_path.c_str(), wmsg.c_str());
					return false;
				}
			} else {
				wlog_error(L"File selected for update %s does not exist anymore, backup not possible", to_path.c_str());
				return false;
			}
		} catch (...) {
			return false;
		}
		++iter;
	}

	return true;
}

bool FileUpdater::is_local_files_changed()
{
	for (auto &file : m_local_manifest) {
		std::string checksum = calculate_files_checksum_safe(file.first);
		if (checksum != file.second) {
			std::wstring checksum_expected = ConvertToUtf16WS(file.second);
			std::wstring checksum_now = ConvertToUtf16WS(checksum);
			wlog_error(L"File %s checksum mismatch after revert, expected %s, now %s", file.first.c_str(), checksum_expected.c_str(), checksum_now.c_str());
			return true;
		}
	}

	log_info("Check of files checksums after revert: passed.");
	return false;
}

bool FileUpdater::is_local_files_updated()
{
	for (manifest_map_t::const_iterator iter = m_manifest.begin(); iter != m_manifest.end(); ++iter) {
		if (iter->second.skip_update) {
			continue;
		}

		std::error_code ec;
		fs::path file_name_part = fs::u8path(iter->first.c_str());
		fs::path to_path(m_app_dir);
		to_path /= file_name_part;

		if (iter->second.remove_at_update) {
			if (fs::exists(to_path, ec)) {
				wlog_error(L"File %s still not exist after update, something went wrong", to_path.c_str());
				return false;
			}
		}

		std::string checksum = calculate_files_checksum_safe(to_path);
		if (checksum != iter->second.hash_sum) {
			log_error("File %s checksum mismatch after an update, expected %s, now %s", iter->first.c_str(), iter->second.hash_sum.c_str(),
				   checksum.c_str());
			return false;
		}
	}

	log_info("Check of files checksums after update: passed.");
	return true;
}