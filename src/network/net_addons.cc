/*
 * Copyright (C) 2020-2021 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "network/net_addons.h"

#include <cassert>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>

#include <boost/format.hpp>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2def.h>
#include <ws2tcpip.h>
#endif

#include "base/i18n.h"
#include "base/log.h"
#include "base/md5.h"
#include "base/warning.h"
#include "graphic/image_cache.h"
#include "io/fileread.h"
#include "io/filesystem/layered_filesystem.h"
#include "io/filewrite.h"
#include "logic/filesystem_constants.h"
#include "network/network.h"
#include "wlapplication_options.h"

namespace AddOns {

/*
 * The networking-related code in this file is inspired by
 * https://www.thecrazyprogrammer.com/2017/06/socket-programming.html
 *
 * The communication protocol is documented in the server
 * repo (widelands/wl_addons_server) in `Server.java`.
 */

namespace {

inline int portable_write(const int socket, const char* buffer, const size_t length) {
#ifdef _WIN32
	return send(socket, buffer, length, 0);
#else
	return write(socket, buffer, length);
#endif
}
inline int portable_read(const int socket, char* buffer, const size_t length) {
#ifdef _WIN32
	return recv(socket, buffer, length, 0);
#else
	return read(socket, buffer, length);
#endif
}

inline void check_string_validity(const std::string& str) {
	if (str.find(' ') != std::string::npos) {
		throw WLWarning("", "String '%s' may not contain whitespaces", str.c_str());
	}
	if (str.find('\n') != std::string::npos) {
		throw WLWarning("", "String '%s' may not contain newlines", str.c_str());
	}
}
}  // namespace

constexpr unsigned kCurrentProtocolVersion = 4;

void NetAddons::init(std::string username, std::string password) {
	if (initialized_) {
		// already initialized
		return;
	}
	if (network_active_) {
		throw WLWarning("", "Network is already active during init");
	}

#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);  // NOLINT
#endif

	if (password.empty()) {
		username = "";
	}
	if (username.empty()) {
		username = last_username_;
	} else {
		last_username_ = "";
	}
	if (password.empty()) {
		password = last_password_;
	} else {
		last_password_ = "";
	}
	check_string_validity(username);

	if ((client_socket_ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		throw WLWarning("", "Unable to create socket");
	}

	const std::string target_ip = get_config_string("addon_server_ip", "widelands.org");
	const int target_port = get_config_int("addon_server_port", 7388);
	sockaddr_in server;
	server.sin_family = AF_INET;
	{
		NetAddress addr;
		// TODO(Nordfriese): inet_addr can't handle IPv6 addresses
		if (!NetAddress::resolve_to_v4(&addr, target_ip, target_port)) {
			throw WLWarning(
			   "", "Unable to resolve host name and port '%s' / %d", target_ip.c_str(), target_port);
		}
		std::ostringstream oss("");
		oss << addr.ip;
		server.sin_addr.s_addr = inet_addr(oss.str().c_str());
		server.sin_port = htons(addr.port);
	}
	if (connect(client_socket_, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
		throw WLWarning("", "Unable to connect to the server");
	}

	std::string send = std::to_string(kCurrentProtocolVersion);
	send += '\n';
	send += i18n::get_locale();
	send += '\n';
	send += username;
	send += "\nENDOFSTREAM\n";
	write_to_server(send);

	is_admin_ = false;
	if (username.empty()) {
		check_endofstream();
	} else {
		std::string data = password;
		data += '\n';
		data += read_line();
		data += '\n';
		check_endofstream();
		SimpleMD5Checksum md5;
		md5.data(data.c_str(), data.size());
		md5.finish_checksum();
		send = md5.get_checksum().str();
		send += "\nENDOFSTREAM\n";
		write_to_server(send);

		data = read_line();
		if (data == "ADMIN") {
			is_admin_ = true;
		} else if (data != "SUCCESS") {
			throw WLWarning("", "Expected login result, received:\n%s", data.c_str());
		}
	}

	initialized_ = true;
	last_username_ = username;
	last_password_ = password;
}

void NetAddons::quit_connection() {
	if (!initialized_) {
		return;
	}
	initialized_ = false;
#ifdef _WIN32
	closesocket(client_socket_);
#else
	close(client_socket_);
#endif

#ifdef SIGPIPE
	signal(SIGPIPE, SIG_DFL);
#endif
}

NetAddons::~NetAddons() {
	quit_connection();
}

void NetAddons::set_login(const std::string& username, const std::string& password) {
	quit_connection();
	if (username.empty()) {
		last_username_ = "";
		last_password_ = "";
	}
	init(username, password);
}

inline void NetAddons::write_to_server(const std::string& send) {
	write_to_server(send.c_str(), send.size());
}
void NetAddons::write_to_server(const char* send, const size_t length) {
	if (portable_write(client_socket_, send, length) >= 0) {
		return;
	}

	std::string message;
	for (; initialized_;) {
		std::string line = read_line();
		if (line.empty()) {
			break;
		}
		message += '\n';
		message += line;
	}

	if (message.empty()) {
		throw WLWarning("", "Connection interrupted (%s)", strerror(errno));
	}
	throw WLWarning("", "Connection interrupted (%s). Reason:%s", strerror(errno), message.c_str());
}

std::string NetAddons::read_line() {
	std::string line;
	char c;
	int n;
	for (;;) {
		n = portable_read(client_socket_, &c, 1);
		if (n != 1 || c == '\n') {
			break;
		}
		line += c;
	}
	return line;
}

void NetAddons::read_file(const int64_t length, const std::string& out) {
	FileWrite fw;
	std::unique_ptr<char[]> buffer(new char[length]);
	int64_t nr_bytes_read = 0;
	do {
		int64_t l = portable_read(client_socket_, buffer.get(), length - nr_bytes_read);
		if (l < 1) {
			throw WLWarning("", "Connection interrupted");
		}
		nr_bytes_read += l;
		fw.data(buffer.get(), l);
	} while (nr_bytes_read < length);
	fw.write(*g_fs, out);
}

void NetAddons::check_endofstream() {
	const std::string text = read_line();
	if (text != "ENDOFSTREAM") {
		throw WLWarning("", "Expected end of stream, received:\n%s", text.c_str());
	}
}

static void check_checksum(const std::string& path, const std::string& checksum) {
	FileRead fr;
	fr.open(*g_fs, path);
	const size_t bytes = fr.get_size();
	std::unique_ptr<char[]> complete(new char[bytes]);
	fr.data_complete(complete.get(), bytes);
	SimpleMD5Checksum md5sum;
	md5sum.data(complete.get(), bytes);
	md5sum.finish_checksum();
	const std::string md5 = md5sum.get_checksum().str();
	if (checksum != md5) {
		throw WLWarning("", "Downloaded file '%s': Checksum mismatch, found %s, expected %s",
		                path.c_str(), md5.c_str(), checksum.c_str());
	}
}

// A crash guard is there to ensure that the socket connection will be reset
// in case an unexpected problem occurs, to prevent subsequent actions from
// reading or writing random leftover bytes. Create it before doing some
// networking stuff and call `ok()` after everything has gone well.
struct CrashGuard {
	explicit CrashGuard(NetAddons& n) : net_(n), ok_(false) {
		assert(net_.initialized_);
		if (net_.network_active_) {
			throw WLWarning("", "Network is already active");
		}
		net_.network_active_ = true;
	}
	void ok() {
		assert(net_.initialized_);
		assert(net_.network_active_);
		assert(!ok_);
		ok_ = true;
	}
	~CrashGuard() {
		assert(net_.initialized_);
		assert(net_.network_active_);
		net_.network_active_ = false;
		if (!ok_) {
			net_.quit_connection();
		}
	}

private:
	NetAddons& net_;
	bool ok_;
};

AddOnsList NetAddons::refresh_remotes() {
	init();

	AddOnsList result_vector;
	int64_t nr_addons;
	{
		CrashGuard guard(*this);
		write_to_server("CMD_LIST\n");

		nr_addons = std::stol(read_line());
		result_vector.resize(nr_addons);
		for (int64_t i = 0; i < nr_addons; ++i) {
			result_vector[i].reset(new AddOnInfo());
			result_vector[i]->internal_name = read_line();
		}
		check_endofstream();
		guard.ok();
	}

	for (int64_t i = 0; i < nr_addons; ++i) {
		try {
			*result_vector[i] = fetch_one_remote(result_vector[i]->internal_name);
		} catch (const std::exception& e) {
			log_err("Skip add-on %s because: %s", result_vector[i]->internal_name.c_str(), e.what());
			result_vector[i]->internal_name = result_vector.back()->internal_name;
			result_vector.pop_back();
			--i;
			--nr_addons;
		}
	}
	return result_vector;
}

AddOnInfo NetAddons::fetch_one_remote(const std::string& name) {
	check_string_validity(name);
	init();
	CrashGuard guard(*this);
	{
		std::string send = "CMD_INFO ";
		send += name;
		send += '\n';
		write_to_server(send);
	}

	AddOnInfo a;
	a.internal_name = name;
	a.unlocalized_descname = read_line();
	const std::string localized_descname = read_line();
	a.descname = [localized_descname]() { return localized_descname; };
	a.unlocalized_description = read_line();
	const std::string localized_description = read_line();
	a.description = [localized_description]() { return localized_description; };
	a.unlocalized_author = read_line();
	const std::string localized_author = read_line();
	a.author = [localized_author]() { return localized_author; };
	a.upload_username = read_line();
	a.version = string_to_version(read_line());
	a.i18n_version = std::stol(read_line());
	a.category = get_category(read_line());

	std::string req = read_line();
	for (; !req.empty();) {
		size_t pos = req.find(',');
		if (pos < req.size()) {
			a.requirements.push_back(req.substr(0, pos));
			req = req.substr(pos + 1);
		} else {
			a.requirements.push_back(req);
			break;
		}
	}

	a.min_wl_version = read_line();
	a.max_wl_version = read_line();
	a.sync_safe = (read_line() == "true");

	for (int j = std::stoi(read_line()); j > 0; --j) {
		const std::string s1 = read_line();
		const std::string s2 = read_line();
		a.screenshots[s1] = s2;
	}
	a.total_file_size = std::stol(read_line());
	a.upload_timestamp = std::stol(read_line());
	a.download_count = std::stol(read_line());
	for (int j = 0; j < kMaxRating; ++j) {
		a.votes[j] = std::stol(read_line());
	}

	const int comments = std::stoi(read_line());
	a.user_comments.resize(comments);
	for (int j = 0; j < comments; ++j) {
		a.user_comments[j].username = read_line();
		a.user_comments[j].timestamp = std::stol(read_line());
		a.user_comments[j].editor = read_line();
		a.user_comments[j].edit_timestamp = std::stol(read_line());
		a.user_comments[j].version = string_to_version(read_line());
		int newlines = std::stoi(read_line());
		a.user_comments[j].message = read_line();
		for (; newlines > 0; --newlines) {
			a.user_comments[j].message += "<br>";
			a.user_comments[j].message += read_line();
		}
	}
	a.verified = read_line() == "verified";

	const std::string icon_checksum = read_line();
	const int64_t icon_file_size = std::stol(read_line());
	if (icon_file_size <= 0) {
		a.icon = g_image_cache->get(kAddOnCategories.at(a.category).icon);
	} else {
		g_fs->ensure_directory_exists(kTempFileDir);
		const std::string path =
		   kTempFileDir + FileSystem::file_separator() + a.internal_name + ".icon" +
		   std::to_string(std::time(nullptr)) /* for disambiguation */ + kTempFileExtension;
		read_file(icon_file_size, path);
		check_checksum(path, icon_checksum);
		a.icon = g_image_cache->get(path);
		g_fs->fs_unlink(path);
	}

	check_endofstream();
	guard.ok();
	return a;
}

void NetAddons::download_addon(const std::string& name,
                               const std::string& save_as,
                               const CallbackFn& progress) {
	check_string_validity(name);
	init();
	CrashGuard guard(*this);
	{
		std::string send = "CMD_DOWNLOAD ";
		send += name;
		send += '\n';
		write_to_server(send);
	}
	g_fs->ensure_directory_exists(save_as);

	const int64_t nr_dirs = std::stol(read_line());
	std::unique_ptr<std::string[]> dirnames(new std::string[nr_dirs]);
	for (int64_t i = 0; i < nr_dirs; ++i) {
		dirnames[i] = read_line();
		g_fs->ensure_directory_exists(save_as + FileSystem::file_separator() + dirnames[i]);
	}
	int64_t progress_state = 0;
	for (int64_t i = -1 /* top-level directory is not counted */; i < nr_dirs; ++i) {
		for (int64_t j = std::stol(read_line()); j > 0; --j) {
			const std::string filename = read_line();
			const std::string checksum = read_line();
			const int64_t length = std::stol(read_line());
			std::string relative_path;
			if (i >= 0) {
				relative_path += dirnames[i];
				relative_path += FileSystem::file_separator();
			}
			relative_path += filename;
			std::string out = save_as;
			out += FileSystem::file_separator();
			out += relative_path;
			FileWrite fw;
			std::unique_ptr<char[]> buffer(new char[length]);
			int64_t nr_bytes_read = 0;
			do {
				progress(relative_path, progress_state);
				int64_t l = portable_read(client_socket_, buffer.get(), length - nr_bytes_read);
				if (l < 1) {
					throw WLWarning("", "Connection interrupted");
				}
				nr_bytes_read += l;
				progress_state += l;
				fw.data(buffer.get(), l);
			} while (nr_bytes_read < length);
			fw.write(*g_fs, out);
			check_checksum(out, checksum);
		}
	}

	check_endofstream();
	guard.ok();
}

void NetAddons::download_i18n(const std::string& name,
                              const std::string& directory,
                              const CallbackFn& progress,
                              const CallbackFn& init_fn) {
	check_string_validity(name);
	init();
	CrashGuard guard(*this);
	{
		std::string send = "CMD_I18N ";
		send += name;
		send += '\n';
		write_to_server(send);
	}
	g_fs->ensure_directory_exists(directory);

	const int64_t nr_translations = std::stol(read_line());
	init_fn("", nr_translations);
	for (int64_t i = 0; i < nr_translations; ++i) {
		const std::string filename = read_line();
		const std::string checksum = read_line();
		progress(filename.substr(0, filename.find('.')), i);
		const int64_t length = std::stol(read_line());

		std::string out = directory;
		out += FileSystem::file_separator();
		out += filename;
		read_file(length, out);
		check_checksum(out, checksum);
	}

	check_endofstream();
	guard.ok();
}

int NetAddons::get_vote(const std::string& addon) {
	check_string_validity(addon);
	int v;
	try {
		init();
		CrashGuard guard(*this);

		std::string send = "CMD_GET_VOTE ";
		send += addon;
		send += '\n';
		write_to_server(send);

		const std::string line = read_line();
		if (line == "NOT_LOGGED_IN") {
			guard.ok();
			return -1;
		}
		v = stoi(line);
		assert(v >= 0);
		assert(v <= kMaxRating);

		check_endofstream();
		guard.ok();
	} catch (...) {
		v = -1;
	}
	return v;
}
void NetAddons::vote(const std::string& addon, const unsigned vote) {
	check_string_validity(addon);
	assert(vote <= kMaxRating);
	init();
	CrashGuard guard(*this);
	std::string send = "CMD_VOTE ";
	send += addon;
	send += ' ';
	send += std::to_string(vote);
	send += '\n';
	write_to_server(send);
	check_endofstream();
	guard.ok();
}
void NetAddons::comment(const AddOnInfo& addon, std::string message, const int64_t index_to_edit) {
	check_string_validity(addon.internal_name);
	init();
	CrashGuard guard(*this);
	std::string send;
	if (index_to_edit < 0) {
		send = "CMD_COMMENT";
	} else {
		send = "CMD_EDIT_COMMENT";
	}
	send += ' ';
	send += addon.internal_name;
	send += ' ';
	send +=
	   (index_to_edit < 0) ? version_to_string(addon.version, false) : std::to_string(index_to_edit);
	send += ' ';

	unsigned nr_lines = 1;
	size_t pos = 0;
	for (;;) {
		pos = message.find('\n', pos);
		if (pos == std::string::npos) {
			break;
		}
		++nr_lines;
		++pos;
	}

	send += std::to_string(nr_lines);
	send += '\n';
	write_to_server(send);

	for (; nr_lines > 1; --nr_lines) {
		pos = message.find('\n');
		assert(pos < std::string::npos);
		++pos;
		write_to_server(message.substr(0, pos));
		message = message.substr(pos);
	}
	write_to_server(message);
	write_to_server("\nENDOFSTREAM\n");

	check_endofstream();
	guard.ok();
}

static size_t gather_addon_content(const std::string& current_dir,
                                   const std::string& prefix,
                                   std::map<std::string, std::set<std::string>>& result) {
	result[prefix] = {};
	size_t nr_files = 0;
	for (const std::string& f : g_fs->list_directory(current_dir)) {
		if (g_fs->is_directory(f)) {
			std::string str = prefix;
			str += FileSystem::file_separator();
			str += FileSystem::fs_filename(f.c_str());
			nr_files += gather_addon_content(f, str, result);
		} else {
			result[prefix].insert(FileSystem::fs_filename(f.c_str()));
			++nr_files;
		}
	}
	return nr_files;
}

void NetAddons::upload_addon(const std::string& name,
                             const CallbackFn& progress,
                             const CallbackFn& init_fn) {
	check_string_validity(name);
	init();

	std::map<std::string /* content */, std::set<std::string> /* files in this directory */> content;
	{
		std::string dir = kAddOnDir;
		dir += FileSystem::file_separator();
		dir += name;
		init_fn("", gather_addon_content(dir, "", content));
	}

	CrashGuard guard(*this);
	std::string send = "CMD_SUBMIT ";
	send += name;
	send += '\n';
	write_to_server(send);

	send = std::to_string(content.size());
	send += '\n';
	for (const auto& pair : content) {
		send += pair.first;
		send += '\n';
	}
	write_to_server(send);

	int64_t state = 0;
	for (const auto& pair : content) {
		send = std::to_string(pair.second.size());
		send += '\n';
		write_to_server(send);
		for (const std::string& file : pair.second) {
			std::string full_path = kAddOnDir;
			full_path += FileSystem::file_separator();
			full_path += name;
			full_path += pair.first;
			full_path += FileSystem::file_separator();
			full_path += file;
			progress(full_path, state++);

			FileRead fr;
			fr.open(*g_fs, full_path);
			const size_t bytes = fr.get_size();
			std::unique_ptr<char[]> complete(new char[bytes]);
			fr.data_complete(complete.get(), bytes);
			SimpleMD5Checksum md5sum;
			md5sum.data(complete.get(), bytes);
			md5sum.finish_checksum();

			send = file;
			send += '\n';
			send += md5sum.get_checksum().str();
			send += '\n';
			send += std::to_string(bytes);
			send += '\n';
			write_to_server(send);
			write_to_server(complete.get(), bytes);
		}
	}
	progress("", state);

	write_to_server("ENDOFSTREAM\n");
	check_endofstream();
	guard.ok();
}

void NetAddons::upload_screenshot(const std::string& addon,
                                  const std::string& image,
                                  const std::string& description) {
	check_string_validity(addon);
	if (description.find('\n') != std::string::npos) {
		throw WLWarning("", "Screenshot descriptions may not contain newlines");
	}
	init();
	CrashGuard guard(*this);
	std::string send = "CMD_SUBMIT_SCREENSHOT ";
	send += addon;
	send += ' ';

	FileRead fr;
	fr.open(*g_fs, image);
	const size_t bytes = fr.get_size();
	std::unique_ptr<char[]> complete(new char[bytes]);
	fr.data_complete(complete.get(), bytes);
	SimpleMD5Checksum md5sum;
	md5sum.data(complete.get(), bytes);
	md5sum.finish_checksum();

	send += std::to_string(bytes);
	send += ' ';
	send += md5sum.get_checksum().str();
	send += ' ';

	unsigned whitespace = 0;
	size_t pos = 0;
	for (;;) {
		pos = description.find(' ', pos);
		if (pos == std::string::npos) {
			break;
		}
		++whitespace;
		++pos;
	}
	send += std::to_string(whitespace);
	send += ' ';
	send += description;
	send += '\n';

	write_to_server(send);
	write_to_server(complete.get(), bytes);
	write_to_server("ENDOFSTREAM\n");

	check_endofstream();
	guard.ok();
}

std::string NetAddons::download_screenshot(const std::string& name, const std::string& screenie) {
	try {
		check_string_validity(name);
		init();
		CrashGuard guard(*this);
		std::string send = "CMD_SCREENSHOT ";
		send += name;
		send += ' ';
		send += screenie;
		send += '\n';
		write_to_server(send);

		std::string temp_dirname =
		   kTempFileDir + FileSystem::file_separator() + name + ".screenshots" + kTempFileExtension;
		g_fs->ensure_directory_exists(temp_dirname);
		const std::string output = temp_dirname + FileSystem::file_separator() + screenie;

		const std::string checksum = read_line();
		const int64_t filesize = stoi(read_line());
		read_file(filesize, output);
		check_checksum(output, checksum);

		check_endofstream();
		guard.ok();

		return output;
	} catch (...) {
		return "";
	}
}

void NetAddons::contact(std::string enquiry) {
	init();
	CrashGuard guard(*this);
	std::string send = "CMD_CONTACT ";

	unsigned nr_lines = 1;
	size_t pos = 0;
	for (;;) {
		pos = enquiry.find('\n', pos);
		if (pos == std::string::npos) {
			break;
		}
		++nr_lines;
		++pos;
	}

	send += std::to_string(nr_lines);
	send += '\n';
	write_to_server(send);

	for (; nr_lines > 1; --nr_lines) {
		pos = enquiry.find('\n');
		assert(pos < std::string::npos);
		++pos;
		write_to_server(enquiry.substr(0, pos));
		enquiry = enquiry.substr(pos);
	}
	write_to_server(enquiry);
	write_to_server("\nENDOFSTREAM\n");

	check_endofstream();
	guard.ok();
}

}  // namespace AddOns
