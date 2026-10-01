//EECS 348 Assignment 2
//Program reads
//Inputs: Sample Text FIle
//Outputs: Displays how many emails are in queue and displays the next email
//Collaboraters: None
//Other Sources: Claude, Gemini
//Author: Fabian Gonzalez
//Creation Date: September 30, 2026
//Revision Date: October 1, 2026

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

// ------------------------------------------------------------------ List ----
// A minimal dynamic array. Elements are MOVED (not copied) when it grows, it
// starts empty (no wasted allocation), and it shrinks when mostly unused so a
// large burst of mail does not pin memory forever.
template <typename T>
class List {
    T*     data_ = nullptr;
    size_t size_ = 0;
    size_t cap_  = 0;

    // Allocate a new block of newCap slots and move the live elements into it.
    void reallocate(size_t newCap) {
        T* fresh = new T[newCap];
        for (size_t i = 0; i < size_; ++i) fresh[i] = std::move(data_[i]);
        delete[] data_;
        data_ = fresh;
        cap_  = newCap;
    }

public:
    List() = default;
    List(const List&)            = delete;   // never needed; avoids accidental deep copies
    List& operator=(const List&) = delete;
    ~List() { delete[] data_; }

    void pushBack(T value) {
        if (size_ == cap_) reallocate(cap_ ? cap_ * 2 : 4);   // double: amortized O(1)
        data_[size_++] = std::move(value);
    }
    void popBack() {
        if (size_ == 0) return;
        data_[--size_] = T();                                 // release the removed element's memory now
        if (cap_ > 16 && size_ < cap_ / 4) reallocate(cap_ / 2);  // shrink when <25% full
    }
    T&       operator[](size_t i)       { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }
    T&       back()                     { return data_[size_ - 1]; }
    size_t   size()  const              { return size_; }
    bool     empty() const              { return size_ == 0; }
};

// --------------------------------------------------------------- MaxHeap ----
// Binary max-heap stored in a List. `Before(a, b)` must return true when `a`
// should be served BEFORE `b`. The heap's root is the element nothing precedes.
// Sifting uses the "hole" technique (move the displaced elements, write the
// moving element once) instead of repeated swaps: fewer moves per level.
template <typename T, typename Before>
class MaxHeap {
    List<T> items_;
    Before  before_;

    static size_t parentOf(size_t i) { return (i - 1) / 2; }

    // Move items_[i] up until its parent no longer comes after it.
    void siftUp(size_t i) {
        T value = std::move(items_[i]);
        while (i > 0) {
            size_t p = parentOf(i);
            if (!before_(value, items_[p])) break;
            items_[i] = std::move(items_[p]);      // pull the parent down into the hole
            i = p;
        }
        items_[i] = std::move(value);
    }

    // Move items_[i] down until it comes before both of its children.
    void siftDown(size_t i) {
        const size_t n = items_.size();
        T value = std::move(items_[i]);
        while (true) {
            size_t child = 2 * i + 1;               // left child
            if (child >= n) break;                  // no children: done
            if (child + 1 < n && before_(items_[child + 1], items_[child]))
                ++child;                            // right child is the better of the two
            if (!before_(items_[child], value)) break;
            items_[i] = std::move(items_[child]);   // pull the better child up into the hole
            i = child;
        }
        items_[i] = std::move(value);
    }

public:
    void insert(T value) {                          // O(log n) worst, O(1) typical
        items_.pushBack(std::move(value));
        siftUp(items_.size() - 1);
    }
    const T& top() const {                          // O(1)
        if (items_.empty()) throw std::out_of_range("MaxHeap::top on empty heap");
        return items_[0];
    }
    void pop() {                                    // O(log n)
        if (items_.empty()) throw std::out_of_range("MaxHeap::pop on empty heap");
        T last = std::move(items_.back());          // take the last leaf...
        items_.popBack();
        if (!items_.empty()) {                      // ...and drop it in at the root
            items_[0] = std::move(last);
            siftDown(0);
        }
    }
    size_t size()  const { return items_.size(); }
    bool   empty() const { return items_.empty(); }
};

// -------------------------------------------------------------- Category ----
// Declared in ascending importance, so a bigger enum value means "read sooner".
enum class Category : std::uint8_t { OtherPerson = 1, ImportantPerson, Peer, Subordinate, Boss };

// Converts between category text and the enum (single place to edit if a new
// category is ever added).
class CategoryInfo {
    struct Entry { const char* name; Category category; };
    static const Entry* table(size_t& count) {
        static const Entry entries[] = {
            {"Boss", Category::Boss},
            {"Subordinate", Category::Subordinate},
            {"Peer", Category::Peer},
            {"ImportantPerson", Category::ImportantPerson},
            {"OtherPerson", Category::OtherPerson},
        };
        count = sizeof(entries) / sizeof(entries[0]);
        return entries;
    }
public:
    // Returns false (and leaves `out` alone) if the text is not a known category.
    static bool parse(const std::string& text, Category& out) {
        size_t n; const Entry* e = table(n);
        for (size_t i = 0; i < n; ++i)
            if (text == e[i].name) { out = e[i].category; return true; }
        return false;
    }
    static const char* name(Category c) {
        size_t n; const Entry* e = table(n);
        for (size_t i = 0; i < n; ++i)
            if (e[i].category == c) return e[i].name;
        return "?";
    }
};

// ------------------------------------------------------------------ Date ----
// Stores a date as the integer YYYYMMDD, so "newer" is just "numerically
// larger". Only 4 bytes, and it rebuilds the MM-DD-YYYY text on demand.
class Date {
    std::uint32_t key_ = 0;

    static bool isLeap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
    static unsigned daysIn(unsigned m, unsigned y) {
        static const unsigned d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return (m == 2 && isLeap(y)) ? 29 : d[m - 1];
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }

public:
    // Strictly validates "MM-DD-YYYY" (including real month lengths / leap years).
    static bool parse(const std::string& s, Date& out) {
        if (s.size() != 10 || s[2] != '-' || s[5] != '-') return false;
        for (size_t i : {0, 1, 3, 4, 6, 7, 8, 9}) if (!digit(s[i])) return false;
        unsigned mm = (s[0] - '0') * 10 + (s[1] - '0');
        unsigned dd = (s[3] - '0') * 10 + (s[4] - '0');
        unsigned yy = (s[6] - '0') * 1000 + (s[7] - '0') * 100 + (s[8] - '0') * 10 + (s[9] - '0');
        if (mm < 1 || mm > 12 || dd < 1 || dd > daysIn(mm, yy)) return false;
        out.key_ = yy * 10000 + mm * 100 + dd;
        return true;
    }
    std::uint32_t key() const { return key_; }
    std::string toString() const {                  // back to MM-DD-YYYY
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02u-%02u-%04u",
                      (key_ / 100) % 100, key_ % 100, key_ / 10000);
        return buf;
    }
};

// ----------------------------------------------------------------- Email ----
// Compact: subject text + 8-byte arrival number + 4-byte date + 1-byte category.
// (The earlier version also stored the category name and date as extra strings.)
class Email {
    std::string   subject_;
    std::uint64_t seq_ = 0;          // arrival order, used only as the final tie-breaker
    Date          date_;
    Category      category_ = Category::OtherPerson;
public:
    Email() = default;
    Email(Category c, std::string subject, Date d, std::uint64_t seq)
        : subject_(std::move(subject)), seq_(seq), date_(d), category_(c) {}

    Category            category() const { return category_; }
    const Date&         date()     const { return date_; }
    std::uint64_t       seq()      const { return seq_; }
    const std::string&  subject()  const { return subject_; }

    // Prints in the exact format the assignment requires.
    void display(std::ostream& out) const {
        out << "Sender: "  << CategoryInfo::name(category_) << '\n'
            << "Subject: " << subject_                      << '\n'
            << "Date: "    << date_.toString()              << '\n';
    }
};

// The ordering rule, kept separate from Email so it can change independently:
//   1) more important category first
//   2) within a category, NEWER date first
//   3) still tied: the email that arrived first goes first (deterministic)
struct EmailPriority {
    bool operator()(const Email& a, const Email& b) const {
        if (a.category() != b.category()) return a.category() > b.category();
        if (a.date().key() != b.date().key()) return a.date().key() > b.date().key();
        return a.seq() < b.seq();
    }
};

// ----------------------------------------------------------------- Inbox ----
// The CEO's unread mail. All priority handling is delegated to the heap.
class Inbox {
    MaxHeap<Email, EmailPriority> unread_;
    std::uint64_t                 nextSeq_ = 0;
public:
    void   receive(Category c, std::string subject, Date d) {
        unread_.insert(Email(c, std::move(subject), d, nextSeq_++));
    }
    bool         hasMail() const { return !unread_.empty(); }
    size_t       count()   const { return unread_.size(); }
    const Email& peek()    const { return unread_.top(); }   // NEXT: look, don't remove
    void         removeTop()     { unread_.pop(); }          // READ: remove
};

// -------------------------------------------------------------- Commands ----
class Command {
public:
    virtual ~Command() {}
    virtual void execute(Inbox& inbox, std::ostream& out) = 0;
};

// EMAIL <category>,<subject>,<date>  -- adds a message to the inbox.
class EmailCommand : public Command {
    Category    category_;
    std::string subject_;
    Date        date_;
public:
    EmailCommand(Category c, std::string s, Date d)
        : category_(c), subject_(std::move(s)), date_(d) {}
    void execute(Inbox& inbox, std::ostream&) override {
        inbox.receive(category_, std::move(subject_), date_);   // commands run once, so move
    }
};

// NEXT -- show the highest-priority email WITHOUT removing it.
class NextCommand : public Command {
public:
    void execute(Inbox& inbox, std::ostream& out) override {
        if (!inbox.hasMail()) { out << "No emails to read.\n"; return; }
        out << "Next email:\n";
        inbox.peek().display(out);
    }
};

// READ -- the CEO has dealt with the top email; remove it (silently).
class ReadCommand : public Command {
public:
    void execute(Inbox& inbox, std::ostream&) override {
        if (inbox.hasMail()) inbox.removeTop();   // READ on an empty inbox is a no-op
    }
};

// COUNT -- how many unread emails remain.
class CountCommand : public Command {
public:
    void execute(Inbox& inbox, std::ostream& out) override {
        out << "There are " << inbox.count() << " emails to read.\n";
    }
};

// -------------------------------------------------------- CommandFactory ----
// Turns one text line into a Command object using a keyword -> builder table.
// To add a command: write its class and add ONE registerCommand line below.
// No parsing code has to change.
class CommandFactory {
    using Builder = std::function<std::unique_ptr<Command>(const std::string& args)>;
    std::unordered_map<std::string, Builder> builders_;

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Builder for commands that take no arguments (NEXT, READ, COUNT).
    template <typename Cmd>
    static Builder noArguments() {
        return [](const std::string& args) -> std::unique_ptr<Command> {
            if (!args.empty()) return nullptr;            // "NEXT junk" is not a valid NEXT
            return std::unique_ptr<Command>(new Cmd());
        };
    }

    // Builder for EMAIL: split "category,subject,date" and validate each part.
    static std::unique_ptr<Command> buildEmail(const std::string& args) {
        size_t first = args.find(',');
        size_t last  = args.rfind(',');                   // last comma, so odd subjects survive
        if (first == std::string::npos || first == last) return nullptr;
        Category cat;
        Date     date;
        if (!CategoryInfo::parse(trim(args.substr(0, first)), cat)) return nullptr;
        if (!Date::parse(trim(args.substr(last + 1)), date))        return nullptr;
        std::string subject = trim(args.substr(first + 1, last - first - 1));
        return std::unique_ptr<Command>(new EmailCommand(cat, std::move(subject), date));
    }

public:
    CommandFactory() {
        builders_["EMAIL"] = &CommandFactory::buildEmail;
        builders_["NEXT"]  = noArguments<NextCommand>();
        builders_["READ"]  = noArguments<ReadCommand>();
        builders_["COUNT"] = noArguments<CountCommand>();
    }

    // Returns nullptr for blank, unknown, or malformed lines (they are skipped).
    std::unique_ptr<Command> parse(const std::string& rawLine) const {
        std::string line = trim(rawLine);                 // also strips Windows '\r'
        if (line.empty()) return nullptr;
        size_t space = line.find_first_of(" \t");
        std::string keyword = line.substr(0, space);
        std::string args    = (space == std::string::npos) ? "" : trim(line.substr(space + 1));
        auto it = builders_.find(keyword);
        if (it == builders_.end()) return nullptr;
        return it->second(args);
    }
};

// -------------------------------------------------------------------- App ----
class App {
    Inbox          inbox_;
    CommandFactory factory_;
public:
    void run(std::istream& in, std::ostream& out) {
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            // Some editors prepend a UTF-8 byte-order mark; ignore it.
            if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            first = false;
            std::unique_ptr<Command> cmd = factory_.parse(line);
            if (cmd) cmd->execute(inbox_, out);
        }
    }
};

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);                     // faster I/O for big files
    App app;
    if (argc > 1) {
        std::ifstream file(argv[1]);
        if (!file) { std::cerr << "Cannot open " << argv[1] << "\n"; return 1; }
        app.run(file, std::cout);
    } else {
        app.run(std::cin, std::cout);
    }
    return 0;
}