#include "rainbow_esp.h"

#include "../fceu.h"

#include <regex>

using easywsclient::WebSocket;

#undef RAINBOW_DEBUG
#define RAINBOW_DEBUG

#ifdef RAINBOW_DEBUG
#define UDBG(...) FCEU_printf(__VA_ARGS__)
#else
#define UDBG(...)
#endif

BrokeStudioFirmware::BrokeStudioFirmware() {
	UDBG("RAINBOW BrokeStudioFirmware ctor\n");

	// Start websocket client
	this->openConnection();

	// Start web server
	this->httpd_run = true;
	char const * const httpd_port = "8080";
	mg_mgr_init(&this->mgr, reinterpret_cast<void*>(this));
	UDBG("Starting web server on port %s\n", httpd_port);
	this->nc = mg_bind(&this->mgr, httpd_port, BrokeStudioFirmware::httpdEvent);
	if (this->nc == NULL) {
		printf("Failed to create web server\n");
	}else {
		mg_set_protocol_http_websocket(this->nc);
		this->httpd_thread = std::thread([this] {
			while (this->httpd_run) {
				mg_mgr_poll(&this->mgr, 1000);
			}
			mg_mgr_free(&this->mgr);
		});
	}
}

BrokeStudioFirmware::~BrokeStudioFirmware() {
	UDBG("RAINBOW BrokeStudioFirmware dtor\n");
	if (this->socket != nullptr) {
		delete this->socket;
		this->socket = nullptr;
	}

	this->httpd_run = false;
	if (this->httpd_thread.joinable()) {
		this->httpd_thread.join();
	}
}

void BrokeStudioFirmware::rx(uint8 v) {
	UDBG("RAINBOW BrokeStudioFirmware rx %02x\n", v);
	if (this->msg_first_byte) {
		this->msg_first_byte = false;
		this->msg_length = v + 1;
	}
	this->rx_buffer.push_back(v);

	if (this->rx_buffer.size() == this->msg_length) {
		this->processBufferedMessage();
		this->msg_first_byte = true;
	}
}

uint8 BrokeStudioFirmware::tx() {
	UDBG("RAINBOW BrokeStudioFirmware tx\n");

	// Refresh buffer from network
	this->receiveDataFromServer();

	// Get byte from buffer
	if (!this->tx_buffer.empty()) {
		last_byte_read = this->tx_buffer.front();
		this->tx_buffer.pop_front();
	}

	UDBG("RAINBOW BrokeStudioFirmware tx <= %02x\n", last_byte_read);
	return last_byte_read;
}

void BrokeStudioFirmware::setGpio15(bool /*v*/) {
}

bool BrokeStudioFirmware::getGpio15() {
	this->receiveDataFromServer();
	return !this->tx_buffer.empty();
}

void BrokeStudioFirmware::processBufferedMessage() {
	assert(this->rx_buffer.size() >= 2); // Buffer must conatain exactly one message, minimal message is two bytes (length + type)
	uint8 const message_size = this->rx_buffer.front();
	assert(message_size >= 1); // minimal payload is one byte (type)
	assert(this->rx_buffer.size() == static_cast<std::deque<uint8>::size_type>(message_size) + 1); // Buffer size must match declared payload size

	// Process the message in RX buffer
	switch (static_cast<n2e_cmds_t>(this->rx_buffer.at(1))) {
		case n2e_cmds_t::GET_ESP_STATUS:
			UDBG("RAIBOW BrokeStudioFirmware received message GET_ESP_STATUS");
			this->tx_buffer.push_back(last_byte_read);
			this->tx_buffer.push_back(1);
			this->tx_buffer.push_back(static_cast<uint8>(e2n_cmds_t::READY));
			break;
		case n2e_cmds_t::DEBUG_LOG:
			#ifdef RAINBOW_DEBUG
				FCEU_printf("RAINBOW DEBUG/LOG: ");
				for (std::deque<uint8>::const_iterator cur = this->rx_buffer.begin() + 2; cur < this->rx_buffer.end(); ++cur) {
					FCEU_printf("%02x ", *cur);
				}
				FCEU_printf("\n");
			#endif
			break;
		case n2e_cmds_t::CLEAR_BUFFERS:
			UDBG("RAINBOW BrokeStudioFirmware received message CLEAR_BUFFERS\n");
			this->tx_buffer.clear();
			this->rx_buffer.clear();
			break;
		case n2e_cmds_t::GET_WIFI_STATUS:
			UDBG("RAINBOW BrokeStudioFirmware received message GET_WIFI_STATUS\n");
			this->tx_buffer.push_back(last_byte_read);
			this->tx_buffer.push_back(2);
			this->tx_buffer.push_back(static_cast<uint8>(e2n_cmds_t::WIFI_STATUS));
			this->tx_buffer.push_back(3); // Simple answer, wifi is ok
			break;
		case n2e_cmds_t::GET_SERVER_STATUS:
			UDBG("RAINBOW BrokeStudioFirmware received message GET_SERVER_STATUS\n");
			this->tx_buffer.push_back(last_byte_read);
			this->tx_buffer.push_back(2);
			this->tx_buffer.push_back(static_cast<uint8>(e2n_cmds_t::SERVER_STATUS));
			this->tx_buffer.push_back(this->socket != nullptr); // Server connection is ok if we succeed to open it
			break;
		case n2e_cmds_t::CONNECT_TO_SERVER:
			UDBG("RAINBOW BrokeStudioFirmware received message CONNECT_TO_SERVER\n");
			this->openConnection();
			break;
		case n2e_cmds_t::DISCONNECT_FROM_SERVER:
			UDBG("RAINBOW BrokeStudioFirmware received message DISCONNECT_FROM_SERVER\n");
			this->closeConnection();
			break;
		case n2e_cmds_t::SEND_MESSAGE_TO_SERVER: {
			UDBG("RAINBOW BrokeStudioFirmware received message SEND_MESSAGE\n");
			uint8 const payload_size = this->rx_buffer.size() - 2;
			std::deque<uint8>::const_iterator payload_begin = this->rx_buffer.begin() + 2;
			std::deque<uint8>::const_iterator payload_end = payload_begin + payload_size;
			this->sendMessageToServer(payload_begin, payload_end);
			break;
		}
		case n2e_cmds_t::FILE_OPEN:
			if (message_size == 3) {
				uint8 const selected_path = this->rx_buffer.at(2);
				uint8 const selected_file = this->rx_buffer.at(3);
				if (selected_path < this->files.size() && selected_file < this->files[selected_path].size()) {
					this->working_path = selected_path;
					this->working_file = selected_file;
					this->file_offset = 0;
					this->file_exists[selected_path][selected_file] = true;
				}
			}
			break;
		case n2e_cmds_t::FILE_CLOSE:
			this->working_file = NO_WORKING_FILE;
			break;
		case n2e_cmds_t::FILE_EXISTS:
			UDBG("RAINBOW BrokeStudioFirmware received message FILE_EXISTS\n");
			if (message_size == 3) {
				uint8 const path = this->rx_buffer.at(2);
				uint8 const file = this->rx_buffer.at(3);
				if (path < this->files.size() && file < this->files[path].size()) {
					this->tx_buffer.push_back(last_byte_read);
					this->tx_buffer.push_back(2);
					this->tx_buffer.push_back(static_cast<uint8>(e2n_cmds_t::FILE_EXISTS));
					this->tx_buffer.push_back(this->file_exists[path][file] ? 1 : 0);
				}
			}
			break;
		case n2e_cmds_t::FILE_DELETE:
			UDBG("RAINBOW BrokeStudioFirmware received message FILE_DELETE\n");
			if (message_size == 3) {
				uint8 const path = this->rx_buffer.at(2);
				uint8 const file = this->rx_buffer.at(3);
				if (path < this->files.size() && file < this->files[path].size()) {
					this->files[path][file].clear();
					this->file_exists[path][file] = false;
				}
			}
			break;
		case n2e_cmds_t::FILE_SET_CUR:
			UDBG("RAINBOW BrokeStudioFirmware received message FILE_SET_CUR\n");
			if (2 <= message_size && message_size <= 5) {
				this->file_offset = this->rx_buffer[2];
				this->file_offset += static_cast<uint32>(message_size >= 3 ? this->rx_buffer[3] : 0) << 8;
				this->file_offset += static_cast<uint32>(message_size >= 4 ? this->rx_buffer[4] : 0) << 16;
				this->file_offset += static_cast<uint32>(message_size >= 5 ? this->rx_buffer[5] : 0) << 24;
			}
			break;
		case n2e_cmds_t::FILE_READ:
			UDBG("RAINBOW BrokeStudioFirmware received message FILE_READ\n");
			if (message_size == 2) {
				if (this->working_file != NO_WORKING_FILE) {
					uint8 const n = this->rx_buffer[2];
					this->readFile(this->working_path, this->working_file, n, this->file_offset);
					this->file_offset += n;
					if (this->file_offset > this->files[this->working_file].size()) {
						this->file_offset = this->files[this->working_file].size();
					}
				}else {
					this->tx_buffer.insert(this->tx_buffer.end(), {last_byte_read, 2, static_cast<uint8>(e2n_cmds_t::FILE_DATA), 0});
				}
			}
			break;
		case n2e_cmds_t::FILE_WRITE:
			UDBG("RAINBOW BrokeStudioFirmware received message FILE_WRITE\n");
			if (message_size >= 3 && this->rx_buffer[2] == message_size - 2 && this->working_file != NO_WORKING_FILE) {
				this->writeFile(this->working_path, this->working_file, this->file_offset, this->rx_buffer.begin() + 3, this->rx_buffer.begin() + message_size + 1);
				this->file_offset += message_size - 2;
			}
			break;
		case n2e_cmds_t::FILE_APPEND:
			UDBG("RAINBOW BrokeStudioFirmware received message FILE_APPEND\n");
			if (message_size >= 3 && this->rx_buffer[2] == message_size - 2 && this->working_file != NO_WORKING_FILE) {
				this->writeFile(this->working_path, this->working_file, this->files[working_path][working_file].size(), this->rx_buffer.begin() + 3, this->rx_buffer.begin() + message_size + 1);
			}
			break;
		case n2e_cmds_t::GET_FILE_LIST:
			UDBG("RAINBOW BrokeStudioFirmware received message GET_FILE_LIST\n");
			if (message_size == 2) {
				UDBG("RAINBOW received FILES.GET_LIST\n");
				std::vector<uint8> existing_files;
				uint8 const path = this->rx_buffer[2];
				assert(this->file_exists[path].size() < 254);
				for (uint8 i = 0; i < this->file_exists[path].size(); ++i) {
					if (this->file_exists[path][i]) {
						existing_files.push_back(i);
					}
				}
				this->tx_buffer.push_back(last_byte_read);
				this->tx_buffer.push_back(existing_files.size() + 2);
				this->tx_buffer.push_back(static_cast<uint8>(e2n_cmds_t::FILE_LIST));
				this->tx_buffer.push_back(existing_files.size());
				for (uint8 i: existing_files) {
					UDBG("RAINBOW => %02x\n", i);
					this->tx_buffer.push_back(i);
				}
			}
			break;
		default:
			UDBG("RAINBOW BrokeStudioFirmware received unknown message %02x\n", this->rx_buffer.at(1));
			break;
	};

	// Remove processed message
	this->rx_buffer.clear();
}

void BrokeStudioFirmware::readFile(uint8 path, uint8 file, uint8 n, uint32 offset) {
	assert(path < NUM_FILE_PATHS);
	assert(file < this->files[path].size());

	// Get data range
	std::vector<uint8> const& f = this->files[path][file];
	std::vector<uint8>::const_iterator data_begin;
	std::vector<uint8>::const_iterator data_end;
	if (offset >= f.size()) {
		data_begin = f.end();
		data_end = data_begin;
	}else {
		data_begin = f.begin() + offset;
		data_end = f.begin() + std::min(static_cast<std::vector<uint8>::size_type>(offset) + n, f.size());
	}
	std::vector<uint8>::size_type const data_size = data_end - data_begin;

	// Write response
	this->tx_buffer.push_back(last_byte_read);
	this->tx_buffer.push_back(data_size + 2);
	this->tx_buffer.push_back(static_cast<uint8>(e2n_cmds_t::FILE_DATA));
	this->tx_buffer.push_back(data_size);
	while (data_begin != data_end) {
		this->tx_buffer.push_back(*data_begin);
		++data_begin;
	}
}

template<class I>
void BrokeStudioFirmware::writeFile(uint8 path, uint8 file, uint32 offset, I data_begin, I data_end) {
	std::vector<uint8>& f = this->files[path][file];
	auto const data_size = data_end - data_begin;
	uint32 const offset_end = offset + data_size;
	if (offset_end > f.size()) {
		f.resize(offset_end, 0);
	}

	for (std::vector<uint8>::size_type i = offset; i < offset_end; ++i) {
		f[i] = *data_begin;
		++data_begin;
	}
	this->file_exists[path][file] = true;
}

template<class I>
void BrokeStudioFirmware::sendMessageToServer(I begin, I end) {
#ifdef RAINBOW_DEBUG
	FCEU_printf("RAINBOW message to send: ");
	for (I cur = begin; cur < end; ++cur) {
		FCEU_printf("%02x ", *cur);
	}
	FCEU_printf("\n");
#endif

	if (this->socket != nullptr) {
		size_t message_size = end - begin;
		std::vector<uint8> aggregated;
		aggregated.reserve(message_size);
		aggregated.insert(aggregated.end(), begin, end);
		this->socket->sendBinary(aggregated);
		this->socket->poll();
	}
}

void BrokeStudioFirmware::receiveDataFromServer() {
	if (this->socket == nullptr) {
		return;
	}

	this->socket->poll();
	this->socket->dispatchBinary([this] (std::vector<uint8_t> const& data) {
		size_t const msg_len = data.end() - data.begin();
		if (msg_len <= 0xff) {
			UDBG("RAINBOW WebSocket data received... size %02x, msg: ", msg_len);
			for (uint8_t const c: data) {
				UDBG("%02x", c);
			}
			UDBG("\n");
			// add last received byte first to match hardware behavior
			// it's more of a mapper thing though ...
			// needs a dummy $5000 read when reading data from buffer
			this->tx_buffer.push_back(last_byte_read);
			this->tx_buffer.push_back(static_cast<uint8>(msg_len+1));
			this->tx_buffer.push_back(static_cast<uint8>(e2n_cmds_t::MESSAGE_FROM_SERVER));
			this->tx_buffer.insert(this->tx_buffer.end(), data.begin(), data.end());
		}
	});
}

void BrokeStudioFirmware::closeConnection() {
	// Do nothing if connection is already closed
	if (this->socket == nullptr) {
		return;
	}

	// Gently ask for connection closing
	if (this->socket->getReadyState() == WebSocket::OPEN) {
		this->socket->close();
	}

	// Start a thread that waits for the connection to be closed, before deleting the socket
	if (this->socket_close_thread.joinable()) {
		this->socket_close_thread.join();
	}
	WebSocket::pointer ws = this->socket;
	this->socket_close_thread = std::thread([ws] {
		while (ws->getReadyState() != WebSocket::CLOSED) {
			ws->poll(5);
		}
		delete ws;
	});

	// Forget about this connection
	this->socket = nullptr;
}

void BrokeStudioFirmware::openConnection() {
	this->closeConnection();

	WebSocket::pointer ws = WebSocket::from_url("ws://localhost:3000");
	if (!ws) {
		UDBG("RAINBOW unable to connect to server");
		return;
	}
	this->socket = ws;
}

std::string BrokeStudioFirmware::pathStrFromIndex(uint8 path, uint8 file) {
	assert(path < NUM_FILE_PATHS);
	std::string const dir_names[NUM_FILE_PATHS] = {"SAVE", "ROMS", "USER"};
	std::ostringstream oss;
	oss << dir_names[path] << "/file" << static_cast<uint16>(file) << ".bin";
	return oss.str();
}

std::pair<uint8, uint8> BrokeStudioFirmware::pathIndexFromStr(std::string const& name) {
	static std::regex const file_path_regex("/?(SAVE|ROMS|USER)/file([0-9]+).bin");
	std::smatch match;
	if (std::regex_match(name, match, file_path_regex)) {
		assert(match.size() == 3);
		uint8 path = 0;
		if (match[1] == "ROMS") {
			path = 1;
		}else if (match[1] == "USER") {
			path = 2;
		}
		std::istringstream iss(match[2]);
		unsigned int file;
		iss >> file;
		if (file <= 0xff) {
			return std::pair<uint8, uint8>(path, file);
		}
	}
	return std::pair<uint8, uint8>(0, NO_WORKING_FILE);
}

void BrokeStudioFirmware::httpdEvent(mg_connection *nc, int ev, void *ev_data) {
	BrokeStudioFirmware* self = reinterpret_cast<BrokeStudioFirmware*>(nc->mgr->user_data);
	auto send_message = [&] (int status_code, char const * body, char const * mime) {
		std::string header = std::string("Content-Type: ") + mime + "\r\nConnection: close\r\n";
		mg_send_response_line(nc, status_code, header.c_str());
		mg_printf(nc, body);
		nc->flags |= MG_F_SEND_AND_CLOSE;
	};
	auto send_generic_error = [&] {
		send_message(400, "<html><body><h1>Error</h1></body><html>\n", "text/html");
	};
	if (ev == MG_EV_HTTP_REQUEST) {
		UDBG("http request event \n");
		struct http_message *hm = (struct http_message *) ev_data;
		UDBG("  uri: %.*s\n", hm->uri.len, hm->uri.p);
		if (strncmp("/api/file/list", hm->uri.p, hm->uri.len) == 0) {
			char path[256];
			int const path_len = mg_get_http_var(&hm->query_string, "path", path, 256);
			if (path_len <= 0) {
				send_generic_error();
				return;
			}
			mg_send_response_line(nc, 200, "Content-Type: application/json\r\nConnection: close\r\n");
			mg_printf(nc, "[\n");
			int id = 0;
			for (uint8_t file_path = 0; file_path < NUM_FILE_PATHS; ++file_path) {
				for (int file_index = 0; file_index < self->file_exists[file_path].size(); ++file_index) {
					if (self->file_exists[file_path][file_index]) {
						mg_printf(nc, "  {\"id\":\"%d\",\"name\":\"%s\",\"size\":\"%d\"}\n", id, BrokeStudioFirmware::pathStrFromIndex(file_path, file_index).c_str(), static_cast<int>(self->files[file_path][file_index].size()));
					}
				}
			}
			mg_printf(nc, "]\n");
			nc->flags |= MG_F_SEND_AND_CLOSE;
		}else if (strncmp("/api/file/delete", hm->uri.p, hm->uri.len) == 0) {
			char filename[256];
			int const path_len = mg_get_http_var(&hm->query_string, "filename", filename, 256);
			if (path_len <= 0) {
				send_generic_error();
				return;
			}
			std::pair<uint8, uint8> path = BrokeStudioFirmware::pathIndexFromStr(filename);
			if (path.second == NO_WORKING_FILE || !self->file_exists[path.first][path.second]) {
				send_message(200, "{\"success\":\"false\"}\n", "application/json");
			}else {
				self->file_exists[path.first][path.second] = false;
				self->files[path.first][path.second].clear();
				send_message(200, "{\"success\":\"true\"}\n", "application/json");
			}
		}else if (strncmp("/api/file/rename", hm->uri.p, hm->uri.len) == 0) {
			char filename[256];
			int const filename_len = mg_get_http_var(&hm->query_string, "filename", filename, 256);
			if (filename_len <= 0) {
				send_generic_error();
				return;
			}
			char new_filename[256];
			int const new_filename_len = mg_get_http_var(&hm->query_string, "newFilename", new_filename, 256);
			if (new_filename_len <= 0) {
				send_generic_error();
				return;
			}

			std::pair<uint8, uint8> path = BrokeStudioFirmware::pathIndexFromStr(filename);
			std::pair<uint8, uint8> new_path = BrokeStudioFirmware::pathIndexFromStr(new_filename);
			if (path.first >= NUM_FILE_PATHS || new_path.first >= NUM_FILE_PATHS || path.second >= 64 || new_path.second >= 64) {
				send_message(200, "{\"success\":\"false\"}\n", "application/json");
				return;
			}

			self->files[new_path.first][new_path.second] = self->files[path.first][path.second];
			self->file_exists[new_path.first][new_path.second] = self->file_exists[path.first][path.second];
			self->file_exists[path.first][path.second] = false;
			self->files[path.first][path.second].clear();

			send_message(200, "{\"success\":\"true\"}\n", "application/json");
		}else if (strncmp("/api/file/download", hm->uri.p, hm->uri.len) == 0) {
			char filename[256];
			int const filename_len = mg_get_http_var(&hm->query_string, "filename", filename, 256);
			if (filename_len <= 0) {
				send_generic_error();
				return;
			}
			std::pair<uint8, uint8> path = BrokeStudioFirmware::pathIndexFromStr(filename);

			if (path.first >= NUM_FILE_PATHS || path.second >= 64 || !self->file_exists[path.first][path.second]) {
				send_generic_error();
				return;
			}
			mg_send_response_line(
				nc, 200,
				"Content-Type: application/octet-stream\r\n"
				"Connection: close\r\n"
			);
			mg_send(nc, self->files[path.first][path.second].data(), self->files[path.first][path.second].size());
			nc->flags |= MG_F_SEND_AND_CLOSE;
		}else if (strncmp("/api/file/upload", hm->uri.p, hm->uri.len) == 0) {
			// Get boundary for multipart form in HTTP headers
			std::string multipart_boundary;
			{
				mg_str const * content_type = mg_get_http_header(hm, "Content-Type");
				if (content_type == NULL) {
					send_generic_error();
					return;
				}
				static std::regex const content_type_regex("multipart/form-data; boundary=(.*)");
				std::smatch match;
				std::string content_type_str(content_type->p, content_type->len);
				if (!std::regex_match(content_type_str, match, content_type_regex)) {
					send_generic_error();
					return;
				}
				assert(match.size() == 2);
				multipart_boundary = match[1];
			}

			// Parse form parts
			std::map<std::string, std::string> params;
			{
				std::string body_str(hm->body.p, hm->body.len);
				std::string::size_type pos = 0;
				while (pos != std::string::npos) {
					// Find the parameter name
					std::string::size_type found_pos = body_str.find("form-data; name=\"", pos);
					if (found_pos == std::string::npos) {
						break;
					}
					pos = found_pos + 17;
					found_pos = body_str.find('"', pos);
					if (found_pos == std::string::npos) {
						break;
					}
					std::string const param_name = body_str.substr(pos, found_pos - pos);
					pos = found_pos;

					// Find the begining of the body
					found_pos = body_str.find("\r\n\r\n", pos);
					if (found_pos == std::string::npos) {
						break;
					}
					pos = found_pos + 4;

					// Find the begining of the next delimiter
					found_pos = body_str.find("\r\n--" + multipart_boundary, pos);
					if (found_pos == std::string::npos) {
						break;
					}
					std::string const param_value = body_str.substr(pos, found_pos - pos);
					pos = found_pos;

					// Store parsed parameter
					params[param_name] = param_value;
				}
			}

			// Process request
			std::map<std::string, std::string>::const_iterator file_data = params.find("file");
			std::map<std::string, std::string>::const_iterator filename = params.find("path");
			if (file_data == params.end() || filename == params.end()) {
				send_generic_error();
				return;
			}
			std::pair<uint8, uint8> path = BrokeStudioFirmware::pathIndexFromStr(filename->second);
			if (path.second == NO_WORKING_FILE) {
				send_generic_error();
				return;
			}
			self->files[path.first][path.second] = std::vector<uint8>(file_data->second.begin(), file_data->second.end());
			self->file_exists[path.first][path.second] = true;

			// Return something webbrowser friendly
			send_message(200, "<html><body><p>Upload success</p></body></html>\n", "text/html");
		}else if (strncmp("/index.html", hm->uri.p, hm->uri.len) == 0) {
			send_message(
				200,
				R"-(<html><body><form action="/api/file/upload" method="post" enctype="multipart/form-data"><input name="file" type="file"><br /><input name="path" type="text" value="/USER/file10.bin"><br /><button type="submit">Upload</button></form></body></html>)-",
				"text/html"
			);
		}else {
			mg_send_response_line(
				nc, 200,
				"Content-Type: text/html\r\n"
				"Connection: close\r\n"
			);
			mg_printf(
				nc,
				"<html><body>\n"
				"<h1>Hello!</h1>\n"
				"<p>Server connection is %s</p>\n"
				"<p>method: %.*s</p>\n"
				"<p>uri: %.*s</p>\n"
				"<p>query: %.*s</p>\n"
				"<p>body:</p>\n"
				"<pre>%.*s</pre>\n"
				"</body></html>\n",
				self->socket != nullptr ? "good" : "bad",
				(int)hm->method.len, hm->method.p,
				(int)hm->uri.len, hm->uri.p,
				(int)hm->query_string.len, hm->query_string.p,
				(int)hm->body.len, hm->body.p
			);
			nc->flags |= MG_F_SEND_AND_CLOSE;
		}
	}
}
