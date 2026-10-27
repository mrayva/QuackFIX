#include "fix_file_reader.hpp"
#include "duckdb/common/exception.hpp"
#include <mutex>

namespace duckdb {

FixFileReader::FixFileReader() : line_number_(0), buffer_offset_(0), file_done_(false), skip_lf_(false) {
}

bool FixFileReader::OpenNextFile(FileSystem &fs, const vector<string> &files, idx_t &file_index, std::mutex &lock) {
	// Close any existing file
	Close();

	// Get next file with lock
	std::lock_guard<std::mutex> guard(lock);

	if (file_index >= files.size()) {
		return false; // No more files
	}

	current_file_ = files[file_index];
	file_index++;

	// Open file using DuckDB FileSystem API (supports S3, HTTP, etc.)
	// Use AUTO_DETECT compression to support .gz and .zst files
	file_handle_ = fs.OpenFile(current_file_, FileOpenFlags(FileOpenFlags::FILE_FLAGS_READ) |
	                                                   FileCompressionType::AUTO_DETECT);
	line_number_ = 0;
	file_done_ = false;
	skip_lf_ = false;
	buffer_.clear();
	buffer_offset_ = 0;

	return true;
}

bool FixFileReader::ReadLine(string &line) {
	if (!file_handle_) {
		return false; // No file open
	}

	line.clear();
	bool found_line = false;
	while (!file_done_) {
		if (buffer_offset_ >= buffer_.size()) {
			buffer_.resize(BUFFER_SIZE);
			idx_t bytes_read = file_handle_->Read((void *)buffer_.data(), BUFFER_SIZE);
			if (bytes_read == 0) {
				file_done_ = true;
				break;
			}
			buffer_.resize(bytes_read);
			buffer_offset_ = 0;
		}

		// A CR may have ended the previous buffer; consume its paired LF here.
		if (skip_lf_) {
			if (buffer_[buffer_offset_] == '\n') {
				buffer_offset_++;
			}
			skip_lf_ = false;
			if (buffer_offset_ >= buffer_.size()) {
				continue;
			}
		}

		size_t newline_pos = buffer_.find_first_of("\r\n", buffer_offset_);
		if (newline_pos == string::npos) {
			line.append(buffer_, buffer_offset_, buffer_.size() - buffer_offset_);
			buffer_offset_ = buffer_.size();
			continue;
		}

		line.append(buffer_, buffer_offset_, newline_pos - buffer_offset_);
		char delimiter = buffer_[newline_pos];
		buffer_offset_ = newline_pos + 1;
		skip_lf_ = delimiter == '\r';
		found_line = true;
		break;
	}

	if (!found_line && (file_done_ || !line.empty())) {
		found_line = !line.empty();
	}
	if (!found_line) {
		return false;
	}
	line_number_++;
	return true;
}

void FixFileReader::Close() {
	file_handle_.reset();
	current_file_.clear();
	line_number_ = 0;
	buffer_.clear();
	buffer_offset_ = 0;
	file_done_ = false;
	skip_lf_ = false;
}

} // namespace duckdb
