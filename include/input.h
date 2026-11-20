#pragma once
#include <string>
#include <vector>

// The input line: editable text, a history ring, and the founding
// criterion - arrows recall commands on the Termux soft keyboard.
class InputLine {
public:
    enum class Act { none, submit, quit, resize };
    Act feed(int ch, std::string* submitted);
    const std::string& text() const { return buf_; }
    size_t cursor() const { return pos_; }
private:
    void load_history();
    std::string buf_;
    size_t pos_ = 0;
    std::vector<std::string> hist_;
    int hidx_ = -1;          // -1 = the live draft, else browsing the ring
    std::string draft_;
    static constexpr size_t kHist = 500;
};
