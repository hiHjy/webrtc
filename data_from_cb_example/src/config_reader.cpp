#include "config_reader.hpp"

#include <cctype>
#include <fstream>
#include <iostream>

ConfigReader::ConfigReader(const std::string &path)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        std::cerr << "failed to open config file: " << path << std::endl;
        return;
    }

    std::string line;
    while (std::getline(input, line))
    {
        const std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned[0] == '#')
        {
            continue;
        }

        const std::size_t split = cleaned.find('=');
        if (split == std::string::npos)
        {
            continue;
        }

        const std::string key = trim(cleaned.substr(0, split));
        const std::string value = trim(cleaned.substr(split + 1));
        if (!key.empty())
        {
            values_[key] = value;
        }
    }

    loaded_ = true;
}

std::string ConfigReader::read(const std::string &key) const
{
    const auto it = values_.find(key);
    if (it == values_.end())
    {
        std::cerr << "missing config key: " << key << std::endl;
        return "";
    }

    return it->second;
}

bool ConfigReader::loaded() const
{
    return loaded_;
}

std::string ConfigReader::trim(const std::string &value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }

    return value.substr(begin, end - begin);
}
