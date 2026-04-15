#ifndef CONFIG_READER_H
#define CONFIG_READER_H

#include <string>
#include <unordered_map>

class ConfigReader
{
public:
    explicit ConfigReader(const std::string &path);
    std::string read(const std::string &key) const;
    bool loaded() const;

private:
    static std::string trim(const std::string &value);

    std::unordered_map<std::string, std::string> values_;
    bool loaded_ = false;
};

#endif
