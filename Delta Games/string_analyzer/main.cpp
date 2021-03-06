#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <filesystem>
#include <mutex>
#include <array>
#include <regex>
#include <map>
#include <deque>

namespace fs = std::experimental::filesystem;

uint64_t string_len = 0;

uint64_t fnv_hash(const char* str)
{
	uint64_t offset = 0xcbf29ce484222325;
	const size_t len = strlen(str);

	for (size_t i = 0; i < len; i++)
	{
		const uint8_t value = str[i];
		offset = offset ^ value;
		offset *= 0x100000001b3;
	}

	return offset;
}

void load_file_into_map_clean(const fs::path& path, std::map<uint64_t, std::string>& arr, std::mutex& mutex)
{
	std::ifstream file(path);

	file.seekg(0, std::ios::end);
	const auto p = file.tellg();
	file.seekg(0, std::ios::beg);

	std::vector<char> buf;
	buf.reserve(p);
	file.read(&buf[0], p);

	static const std::regex str_regex(R"(0x[a-z0-9]{1,12} \([0-9]{1,5}\): )");

	std::stringstream tmp;
	tmp << std::regex_replace(&buf[0], str_regex, "");

	std::string line;
	line.reserve(32768);
	while (std::getline(tmp, line))
	{
		std::remove_if(line.begin(), line.end(), [](char c)
		{
			return c < 0 || c == '\t' || c == '\n' || c == '\r';
		});

		if (!line.empty() && line.length() >= string_len)
		{
			std::lock_guard<std::mutex> lock_guard(mutex);

			arr.insert_or_assign(fnv_hash(line.c_str()), line);
		}
	}

	std::stringstream ss;
	ss << "Finished " << path << '\n';

	std::cout << ss.str();
}

void load_file_into_map_dirty(const fs::path& path, std::map<uint64_t, std::string>& arr)
{
	std::ifstream file(path);

	file.seekg(0, std::ios::end);
	const auto p = file.tellg();
	file.seekg(0, std::ios::beg);

	std::vector<char> buf;
	buf.reserve(p);
	file.read(&buf[0], p);

	static const std::regex str_regex(R"(0x[a-z0-9]{1,12} \([0-9]{1,5}\): )");

	std::stringstream tmp;
	tmp << std::regex_replace(&buf[0], str_regex, "");

	std::string line;
	line.reserve(32768);
	while (std::getline(tmp, line))
	{
		std::remove_if(line.begin(), line.end(), [](char c)
		{
			return c < 0 || c == '\t' || c == '\n' || c == '\r';
		});

		if (!line.empty() && line.length() >= string_len)
			arr.insert_or_assign(fnv_hash(line.c_str()), line);
	}

	std::stringstream ss;
	ss << "Finished " << path << '\n';

	std::cout << ss.str();
}

void threaded_clean_file_loading(const std::vector<fs::path>& file_list, std::map<uint64_t, std::string>& arr, std::mutex& mutex)
{
	std::vector<std::thread> threads;

	for (const auto& x : file_list)
		threads.push_back(std::thread(load_file_into_map_clean, std::cref(x), std::ref(arr), std::ref(mutex)));

	for (auto& x : threads)
		x.join();
}

void threaded_dirty_file_loading(const std::vector<fs::path>& file_list, std::deque<std::map<uint64_t, std::string>>& vec_map_mutex)
{
	std::vector<std::thread> threads;

	for (size_t i = 0; i < file_list.size(); i++)
		threads.push_back(std::thread(load_file_into_map_dirty, std::cref(file_list.at(i)), std::ref(vec_map_mutex.at(i))));

	for (auto& x : threads)
		x.join();
}

void load_files(std::map<uint64_t, std::string>& clean_strs, std::map<uint64_t, std::string>& dirty_strs)
{
	const fs::path clean_dir(fs::current_path() / "clean/");
	std::vector<fs::path> clean_paths;
	std::mutex clean_mutex;

	auto load_clean_files = [&]()
	{
		for (const auto& p : fs::recursive_directory_iterator(clean_dir))
		{
			fs::path cur_file(p);
		
			if (is_regular_file(cur_file) && cur_file.extension() == ".txt")
				clean_paths.push_back(std::move(cur_file));
		}

		std::stringstream ss;
		ss << "Loading " << clean_paths.size() << " clean files." << '\n';
		std::cout << ss.str();

		threaded_clean_file_loading(clean_paths, clean_strs, clean_mutex);
	};

	const fs::path dirty_dir(fs::current_path() / "dirty/");

	std::vector<fs::path> dirty_paths;
	std::deque<std::map<uint64_t, std::string>> deq_map;

	auto load_dirty_files = [&]()
	{
		for (const auto& p : fs::recursive_directory_iterator(dirty_dir))
		{
			fs::path cur_file(p);
	
			if (is_regular_file(cur_file) && cur_file.extension() == ".txt")
				dirty_paths.push_back(std::move(cur_file));
		}

		std::stringstream ss;
		ss << "Loading " << dirty_paths.size() << " dirty files." << '\n';
		std::cout << ss.str();

		deq_map.resize(dirty_paths.size());
	
		threaded_dirty_file_loading(dirty_paths, deq_map);
	};

	std::array<std::thread, 2> file_loading_thread;
	file_loading_thread[0] = std::thread(load_clean_files);
	file_loading_thread[1] = std::thread(load_dirty_files);

	for (auto& x : file_loading_thread)
		x.join();

	std::cout << "FIltering false detections... ";

	// Key_hash, Bool List
	std::map<uint64_t, std::vector<bool>> keys_checked;
	std::mutex key_mutex;

	std::vector<std::thread> threadpool;

	auto search = [&](const std::map<uint64_t, std::string>& map)
	{
		// pair in map
		for (const auto& x : map)
		{
			// see if we have hash loaded
			const auto clean_it = keys_checked.find(x.first);

			// if we do, push back another true bool
			if (clean_it != keys_checked.end())
			{
				std::lock_guard<std::mutex> lock_guard(key_mutex);
				keys_checked.at(x.first).push_back(true);
			}
			else // else start a new vector
			{
				std::lock_guard<std::mutex> lock_guard(key_mutex);
				keys_checked.try_emplace(x.first, std::vector<bool>{ true });
			}
		}
	};

	for (const auto& map : deq_map)
		threadpool.push_back(std::thread(search, map));

	for (auto& x : threadpool)
		x.join();

	// Loop through 
	for (const auto& key : keys_checked)
	{
		if (key.second.size() == dirty_paths.size() && std::all_of(key.second.begin(), key.second.end(), [](bool b) { return b == true; }))
		{
			for (const auto& map : deq_map)
			{
				const auto& it = map.find(key.first);
				if (it != map.end())
					dirty_strs.try_emplace(key.first, (*it).second);
			}
		}
	}

	std::cout << "Finished." << std::endl;
}

int main()
{
	std::map<uint64_t, std::string> map_clean_str;
	std::map<uint64_t, std::string> map_dirty_str;

	std::cout << "String Analyzer by phage" << std::endl;
	std::cout << "Please specify the minimum length of strings you'd like to check for." << std::endl << "Length: ";
	std::cin >> string_len;

	load_files(map_clean_str, map_dirty_str);

	std::cout << "Processing " << map_clean_str.size() << " clean and " << map_dirty_str.size() << " dirty strings." << std::endl;

	for (auto it = map_dirty_str.begin(); it != map_dirty_str.end();)
	{
		const auto clean_it = map_clean_str.find(it->first);

		if (clean_it != map_clean_str.end())
		{
			it = map_dirty_str.erase(it);
		}
		else
			++it;
	}

	std::cout << "Writing " << map_dirty_str.size() << " suspicious strings... ";

	FILE* file = nullptr;
	fopen_s(&file, "non_duplicate.txt", "w");
	if (!file)
	{
		std::cout << "Opening file failed." << std::endl;
		system("pause");

		return 1;
	}

	for (const auto& x : map_dirty_str)
	{
		if (x.second.size() < string_len)
			continue;

		if (!all_of(x.second.begin(), x.second.end(), ::isgraph))
			continue;

		fprintf(file, "%s\n", x.second.c_str());
	}

	std::cout << "Finished." << std::endl;

	fclose(file);
	
	system("pause");

	return 0;
}
