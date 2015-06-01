#include "btree_dir.h"

#include <vector>
#include <algorithm>
#include <utility>
#include <type_traits>
#include <assert.h>
#include <iterator>
#include <stdio.h>

static void dir_check(bool condition)
{
    if (!condition)
        throw securefs::CorruptedDirectoryException();
}

namespace securefs
{
template <class T>
static const byte* read_and_forward(const byte* buffer, const byte* end, T& value)
{
    static_assert(std::is_trivially_copyable<T>::value, "");
    dir_check(buffer + sizeof(value) <= end);
    memcpy(&value, buffer, sizeof(value));
    return buffer + sizeof(value);
}

template <class T>
static byte* write_and_forward(const T& value, byte* buffer, const byte* end)
{
    static_assert(std::is_trivially_copyable<T>::value, "");
    dir_check(buffer + sizeof(value) <= end);
    memcpy(buffer, &value, sizeof(value));
    return buffer + sizeof(value);
}

template <class T>
static T read_little_endian_and_forward(const byte** buffer, const byte* end)
{
    dir_check(*buffer + sizeof(T) <= end);
    auto v = from_little_endian<T>(*buffer);
    *buffer += sizeof(T);
    return v;
}

template <class T>
static byte* write_little_endian_and_forward(const T& value, byte* buffer, const byte* end)
{
    dir_check(buffer + sizeof(T) <= end);
    to_little_endian(value, buffer);
    return buffer + sizeof(T);
}

template <class Container>
static void slice(Container& c1, Container& c2, size_t index)
{
    typedef std::move_iterator<typename Container::iterator> iterator;
    c2.assign(iterator(c1.begin() + index), iterator(c1.end()));
    c1.erase(c1.begin() + index, c1.end());
}

template <class Container>
static void steal(Container& c1, Container& c2)
{
    typedef std::move_iterator<typename Container::iterator> iterator;
    c1.insert(c1.end(), iterator(c2.begin()), iterator(c2.end()));
}

template <class Container>
static void steal(Container& c1, const Container& c2)
{
    c1.insert(c1.end(), c2.begin(), c2.end());
}

template <class Container>
typename Container::iterator get_iter(Container& c, size_t index)
{
    if (index >= c.size())
        throw std::out_of_range("index out of range");
    return std::next(c.begin(), index);
}

template <class Container>
typename Container::const_iterator get_iter(const Container& c, size_t index)
{
    if (index >= c.size())
        throw std::out_of_range("index out of range");
    return std::next(c.begin(), index);
}

class BtreeDirectory::FreePage
{
public:
    uint32_t next;
    uint32_t prev;
};

void BtreeDirectory::read_free_page(uint32_t num, FreePage& fp)
{
    byte buffer[BLOCK_SIZE];
    dir_check(m_stream->read(buffer, num * BLOCK_SIZE, BLOCK_SIZE) == BLOCK_SIZE);
    if (from_little_endian<uint32_t>(buffer) != 0)
        throw CorruptedDirectoryException();
    fp.next = from_little_endian<uint32_t>(buffer + sizeof(uint32_t));
    fp.prev = from_little_endian<uint32_t>(buffer + sizeof(uint32_t) * 2);
}

void BtreeDirectory::write_free_page(uint32_t num, const FreePage& fp)
{
    byte buffer[BLOCK_SIZE] = {};
    to_little_endian(fp.next, buffer + sizeof(uint32_t));
    to_little_endian(fp.prev, buffer + sizeof(uint32_t) * 2);
    m_stream->write(buffer, BLOCK_SIZE * num, BLOCK_SIZE);
}

uint32_t BtreeDirectory::allocate_page()
{
    auto pg = get_start_free_page();
    if (pg == INVALID_PAGE)
    {
        auto result = static_cast<uint32_t>(m_stream->size() / BLOCK_SIZE);
        m_stream->resize(m_stream->size() + BLOCK_SIZE);
        return result;
    }
    FreePage fp;
    read_free_page(pg, fp);
    set_num_free_page(get_num_free_page() - 1);
    set_start_free_page(fp.next);
    if (fp.next != INVALID_PAGE)
    {
        read_free_page(fp.next, fp);
        fp.prev = INVALID_PAGE;
        write_free_page(get_start_free_page(), fp);
    }
    return pg;
}

void BtreeDirectory::deallocate_page(uint32_t num)
{
    if (num * BLOCK_SIZE == this->m_stream->size())
    {
        this->m_stream->resize((num - 1) * BLOCK_SIZE);
        return;    // Special case where the stream can be shrinked
    }

    // Otherwise add the page to free list
    FreePage fp;
    fp.prev = INVALID_PAGE;
    fp.next = get_start_free_page();
    write_free_page(num, fp);

    auto start = get_start_free_page();
    if (start != INVALID_PAGE)
    {
        read_free_page(start, fp);
        fp.prev = num;
        write_free_page(start, fp);
    }
    set_start_free_page(num);
    set_num_free_page(get_num_free_page() + 1);
}

void BtreeNode::from_buffer(const byte* buffer, size_t size)
{
    const byte* end_of_buffer = buffer + size;

    auto flag = read_little_endian_and_forward<uint32_t>(&buffer, end_of_buffer);
    if (flag == 0)
        return;
    auto child_num = read_little_endian_and_forward<uint16_t>(&buffer, end_of_buffer);
    auto entry_num = read_little_endian_and_forward<uint16_t>(&buffer, end_of_buffer);

    for (uint16_t i = 0; i < child_num; ++i)
    {
        m_child_indices.push_back(read_little_endian_and_forward<uint32_t>(&buffer, end_of_buffer));
    }
    DirEntry e;
    for (uint16_t i = 0; i < entry_num; ++i)
    {
        std::array<char, Directory::MAX_FILENAME_LENGTH + 1> filename;
        buffer = read_and_forward(buffer, end_of_buffer, filename);
        buffer = read_and_forward(buffer, end_of_buffer, e.id);
        buffer = read_and_forward(buffer, end_of_buffer, e.type);
        filename.back() = 0;
        e.filename = filename.data();
        m_entries.push_back(std::move(e));
    }
}

void BtreeNode::to_buffer(byte* buffer, size_t size) const
{
    const byte* end_of_buffer = buffer + size;
    buffer = write_little_endian_and_forward(static_cast<uint32_t>(1), buffer, end_of_buffer);
    buffer = write_little_endian_and_forward(
        static_cast<uint16_t>(m_child_indices.size()), buffer, end_of_buffer);
    buffer = write_little_endian_and_forward(
        static_cast<uint16_t>(m_entries.size()), buffer, end_of_buffer);

    for (uint32_t index : m_child_indices)
    {
        buffer = write_little_endian_and_forward(index, buffer, end_of_buffer);
    }

    for (auto&& e : m_entries)
    {
        std::array<char, Directory::MAX_FILENAME_LENGTH + 1> filename;
        if (e.filename.size() > Directory::MAX_FILENAME_LENGTH)
            throw OSException(ENAMETOOLONG);
        std::copy(e.filename.begin(), e.filename.end(), filename.begin());
        filename[e.filename.size()] = 0;
        buffer = write_and_forward(filename, buffer, end_of_buffer);
        buffer = write_and_forward(e.id, buffer, end_of_buffer);
        buffer = write_and_forward(e.type, buffer, end_of_buffer);
    }
}

BtreeDirectory::~BtreeDirectory()
{
    try
    {
        flush_cache();
    }
    catch (...)
    {
    }
}

void BtreeDirectory::flush_cache()
{
    for (auto&& pair : m_node_cache)
    {
        auto&& n = *pair.second;
        if (n.is_dirty())
        {
            write_node(n.page_number(), n);
            n.clear_dirty();
        }
    }
}

bool BtreeDirectory::validate_node(const BtreeNode* n, int depth)
{
    if (depth > BTREE_MAX_DEPTH)
        return false;
    if (!std::is_sorted(n->entries().begin(), n->entries().end()))
        return false;
    if (n->parent_page_number() != INVALID_PAGE
        && (n->entries().size() < MAX_NUM_ENTRIES / 2 || n->entries().size() > MAX_NUM_ENTRIES))
        return false;
    if (!n->is_leaf())
    {
        for (size_t i = 0, size = n->entries().size(); i < size; ++i)
        {
            const Entry& e = n->entries()[i];
            const Node* lchild = retrieve_node(n->page_number(), n->children()[i]);
            const Node* rchild = retrieve_node(n->page_number(), n->children()[i + 1]);
            validate_node(lchild, depth + 1);
            validate_node(rchild, depth + 1);
            if (e < lchild->entries().back() || rchild->entries().front() < e)
                return false;
        }
    }
    return true;
}

void BtreeDirectory::clear_cache() { m_node_cache.clear(); }

BtreeNode* BtreeDirectory::retrieve_existing_node(uint32_t num)
{
    auto iter = m_node_cache.find(num);
    if (iter == m_node_cache.end())
        return nullptr;
    return iter->second.get();
}

BtreeDirectory::Node* BtreeDirectory::retrieve_node(uint32_t parent_num, uint32_t num)
{
    auto iter = m_node_cache.find(num);
    if (iter != m_node_cache.end())
    {
        auto n = iter->second.get();
        dir_check(parent_num == INVALID_PAGE || parent_num == n->parent_page_number());
        return n;
    }
    std::unique_ptr<Node> n(new Node(parent_num, num));
    read_node(num, *n);
    auto result = n.get();
    m_node_cache.emplace(num, std::move(n));
    return result;
}

void BtreeDirectory::subflush() { flush_cache(); }

std::tuple<BtreeNode*, ptrdiff_t, bool> BtreeDirectory::find_node(const std::string& name)
{
    BtreeNode* n = get_root_node();
    if (!n)
        return std::make_tuple(nullptr, 0, false);
    for (int i = 0; i < BTREE_MAX_DEPTH; ++i)
    {
        auto iter = std::lower_bound(n->entries().begin(), n->entries().end(), name);
        if (iter != n->entries().end() && name == iter->filename)
            return std::make_tuple(n, iter - n->entries().begin(), true);
        if (n->is_leaf())
            return std::make_tuple(n, iter - n->entries().begin(), false);
        n = retrieve_node(n->page_number(), n->children().at(iter - n->entries().begin()));
    }
    throw CorruptedDirectoryException();    // A loop is present in the "tree" structure
}

BtreeNode* BtreeDirectory::get_root_node()
{
    auto pg = get_root_page();
    if (pg == INVALID_PAGE)
        return nullptr;
    return retrieve_node(INVALID_PAGE, pg);
}

bool BtreeDirectory::get_entry(const std::string& name, id_type& id, int& type)
{
    if (name.size() > MAX_FILENAME_LENGTH)
        throw OSException(ENAMETOOLONG);

    BtreeNode* node;
    ptrdiff_t entry_index;
    bool is_equal;
    std::tie(node, entry_index, is_equal) = find_node(name);

    if (!is_equal || !node)
        return false;
    const Entry& e = node->entries().at(entry_index);
    if (name == e.filename)
    {
        id = e.id;
        type = e.type;
        return true;
    }
    return false;
}

void BtreeDirectory::read_node(uint32_t num, BtreeDirectory::Node& n)
{
    if (num == INVALID_PAGE)
        throw CorruptedDirectoryException();
    byte buffer[BLOCK_SIZE];
    dir_check(m_stream->read(buffer, num * BLOCK_SIZE, BLOCK_SIZE) == BLOCK_SIZE);
    n.from_buffer(buffer, sizeof(buffer));
}

void BtreeDirectory::write_node(uint32_t num, const BtreeDirectory::Node& n)
{
    if (num == INVALID_PAGE)
        throw CorruptedDirectoryException();
    byte buffer[BLOCK_SIZE];
    n.to_buffer(buffer, sizeof(buffer));
    m_stream->write(buffer, num * BLOCK_SIZE, BLOCK_SIZE);
}

bool BtreeDirectory::add_entry(const std::string& name, const id_type& id, int type)
{
    if (name.size() > MAX_FILENAME_LENGTH)
        throw OSException(ENAMETOOLONG);

    BtreeNode* node;
    bool is_equal;
    std::tie(node, std::ignore, is_equal) = find_node(name);

    if (is_equal)
    {
        return false;
    }

    if (!node)
    {
        set_root_page(allocate_page());
        node = get_root_node();
        node->mutable_entries().emplace_back(Entry{name, id, static_cast<uint32_t>(type)});
        return true;
    }
    insert_and_balance(node, Entry{name, id, static_cast<uint32_t>(type)}, INVALID_PAGE, 0);
    return true;
}

void BtreeDirectory::adjust_children_in_cache(BtreeNode* n, uint32_t parent)
{
    for (uint32_t c : n->children())
    {
        auto child = retrieve_existing_node(c);
        if (child)
            child->mutable_parent_page_number() = parent;
    }
}

// This function assumes that every parent node of n is already in the cache
void BtreeDirectory::insert_and_balance(BtreeNode* n, Entry e, uint32_t additional_child, int depth)
{
    dir_check(depth < BTREE_MAX_DEPTH);
    auto iter = std::lower_bound(n->entries().begin(), n->entries().end(), e);
    if (additional_child != INVALID_PAGE && !n->is_leaf())
        n->mutable_children().insert(n->children().begin() + (iter - n->entries().begin()) + 1,
                                     additional_child);
    n->mutable_entries().insert(iter, std::move(e));
    if (n->entries().size() > MAX_NUM_ENTRIES)
    {
        Node* sibling = retrieve_node(n->parent_page_number(), allocate_page());
        auto middle_index = n->entries().size() / 2 - 1;
        e = std::move(n->mutable_entries()[middle_index]);
        if (!n->is_leaf())
        {
            slice(n->mutable_children(), sibling->mutable_children(), middle_index + 1);
            adjust_children_in_cache(sibling);
        }
        slice(n->mutable_entries(), sibling->mutable_entries(), middle_index + 1);
        n->mutable_entries().pop_back();
        if (n->parent_page_number() == INVALID_PAGE)
        {
            auto new_root_page = allocate_page();
            Node* root = retrieve_node(INVALID_PAGE, new_root_page);
            root->mutable_children().push_back(n->page_number());
            root->mutable_children().push_back(sibling->page_number());
            root->mutable_entries().push_back(std::move(e));
            set_root_page(new_root_page);
            n->mutable_parent_page_number() = new_root_page;
            sibling->mutable_parent_page_number() = new_root_page;
        }
        else
        {
            insert_and_balance(retrieve_existing_node(n->parent_page_number()),
                               std::move(e),
                               sibling->page_number(),
                               depth + 1);
        }
    }
}

BtreeNode* BtreeDirectory::replace_with_sub_entry(BtreeNode* n, ptrdiff_t index, int depth)
{
    dir_check(depth < BTREE_MAX_DEPTH);
    if (n->is_leaf())
    {
        n->mutable_entries().erase(n->entries().begin() + index);
        return n;
    }
    else
    {
        Node* lchild = retrieve_node(n->page_number(), n->children().at(index));
        while (!lchild->is_leaf())
            lchild = retrieve_node(lchild->page_number(), lchild->children().back());
        n->mutable_entries().at(index) = std::move(lchild->mutable_entries().back());
        lchild->mutable_entries().pop_back();
        return lchild;
    }
}

void BtreeDirectory::del_node(BtreeNode* n)
{
    if (!n)
        return;
    deallocate_page(n->page_number());
    m_node_cache.erase(n->page_number());
}

std::pair<ptrdiff_t, BtreeNode*> BtreeDirectory::find_sibling(const BtreeNode* parent,
                                                              const BtreeNode* node)
{
    dir_check(parent->page_number() == node->parent_page_number());
    const auto& child_indices = parent->children();
    auto iter = std::find(child_indices.begin(), child_indices.end(), node->page_number());
    auto index = iter - child_indices.begin();
    dir_check(iter != child_indices.end());

    if (iter + 1 == child_indices.end())
        return std::make_pair(
            index - 1, retrieve_node(parent->page_number(), parent->children().at(index - 1)));

    return std::make_pair(index,
                          retrieve_node(parent->page_number(), parent->children().at(index + 1)));
}

void BtreeDirectory::rotate(BtreeNode* left, BtreeNode* right, Entry& separator)
{
    std::vector<Entry> temp_entries;
    temp_entries.reserve(left->entries().size() + right->entries().size() + 1);

    typedef std::move_iterator<decltype(temp_entries.begin())> entry_move_iterator;

    steal(temp_entries, left->mutable_entries());
    temp_entries.push_back(std::move(separator));
    steal(temp_entries, right->mutable_entries());

    auto middle = temp_entries.size() / 2;
    separator = std::move(temp_entries.at(middle));
    left->mutable_entries().assign(entry_move_iterator(temp_entries.begin()),
                                   entry_move_iterator(temp_entries.begin() + middle));
    right->mutable_entries().assign(entry_move_iterator(temp_entries.begin() + middle + 1),
                                    entry_move_iterator(temp_entries.end()));

    if (!left->children().empty() && !right->children().empty())
    {
        std::vector<uint32_t> temp_children;
        temp_children.reserve(left->children().size() + right->children().size());
        steal(temp_children, left->children());
        steal(temp_children, right->children());

        left->mutable_children().assign(temp_children.begin(), temp_children.begin() + middle + 1);
        right->mutable_children().assign(temp_children.begin() + middle + 1, temp_children.end());
        adjust_children_in_cache(left);
        adjust_children_in_cache(right);
    }
}

void BtreeDirectory::merge(BtreeNode* left,
                           BtreeNode* right,
                           BtreeNode* parent,
                           ptrdiff_t entry_index)
{
    left->mutable_entries().push_back(std::move(parent->mutable_entries().at(entry_index)));
    parent->mutable_entries().erase(parent->entries().begin() + entry_index);
    parent->mutable_children().erase(
        std::find(parent->children().begin(), parent->children().end(), right->page_number()));
    steal(left->mutable_entries(), right->mutable_entries());
    this->adjust_children_in_cache(right, left->page_number());
    steal(left->mutable_children(), right->mutable_children());
    this->del_node(right);
}

// This funciton assumes that every parent node is in the cache
void BtreeDirectory::balance_up(BtreeNode* n, int depth)
{
    dir_check(depth < BTREE_MAX_DEPTH);

    if (n->parent_page_number() == INVALID_PAGE && n->entries().empty() && !n->children().empty())
    {
        dir_check(n->children().size() == 1);
        adjust_children_in_cache(n, INVALID_PAGE);
        set_root_page(n->children().front());
        del_node(n);
        return;
    }
    if (n->parent_page_number() == INVALID_PAGE || n->entries().size() >= MAX_NUM_ENTRIES / 2)
        return;

    Node* parent = retrieve_existing_node(n->parent_page_number());
    dir_check(parent);

    ptrdiff_t entry_index;
    BtreeNode* sibling;
    std::tie(entry_index, sibling) = find_sibling(parent, n);

    if (n->entries().size() + sibling->entries().size() < MAX_NUM_ENTRIES)
    {
        if (n->entries().back() < sibling->entries().front())
            merge(n, sibling, parent, entry_index);
        else
            merge(sibling, n, parent, entry_index);
    }
    else
    {
        if (n->entries().back() < sibling->entries().front())
            rotate(n, sibling, parent->mutable_entries().at(entry_index));
        else
            rotate(sibling, n, parent->mutable_entries().at(entry_index));
    }

    balance_up(parent, depth + 1);
}

bool BtreeDirectory::remove_entry(const std::string& name, id_type& id, int& type)
{
    if (name.size() > MAX_FILENAME_LENGTH)
        throw OSException(ENAMETOOLONG);

    BtreeNode* node;
    ptrdiff_t entry_index;
    bool is_equal;
    std::tie(node, entry_index, is_equal) = find_node(name);

    if (!is_equal || !node)
        return false;
    const Entry& e = node->entries().at(entry_index);

    id = e.id;
    type = e.type;
    auto leaf_node = replace_with_sub_entry(node, entry_index, 0);
    balance_up(leaf_node, 0);
    return true;
}

bool BtreeDirectory::validate_free_list()
{
    auto pg = get_start_free_page();
    uint32_t prev = INVALID_PAGE;
    FreePage fp;
    for (size_t i = 0; i < get_num_free_page(); ++i)
    {
        read_free_page(pg, fp);
        if (fp.prev != prev)
            return false;
        prev = pg;
        pg = fp.next;
    }
    return pg == INVALID_PAGE;
}

bool BtreeDirectory::validate_btree_structure()
{
    Node* root = get_root_node();
    if (root)
        return validate_node(root, 0);
    return true;
}

void BtreeDirectory::to_dot_graph(const char* filename)
{
    auto root = get_root_node();
    if (!root)
        return;
    FILE* fp = fopen(filename, "w");
    if (!fp)
        throw OSException(errno);
    fputs("digraph Btree{\nrankdir=BT;\n", fp);
    write_dot_graph(root, fp);
    fputs("\n}\n", fp);
    if (feof(fp))
    {
        OSException err(errno);
        fclose(fp);
        throw err;
    }
    fclose(fp);
}

static std::string to_string(const std::vector<DirEntry>& entries)
{
    std::string result;
    for (auto&& e : entries)
    {
        result += e.filename;
        result += '\n';
    }
    return result;
}

void BtreeDirectory::write_dot_graph(const BtreeNode* n, FILE* fp)
{
    if (n->parent_page_number() != INVALID_PAGE)
        fprintf(fp,
                "    node%u -> node%u [style=dotted];\n",
                n->page_number(),
                n->parent_page_number());
    fprintf(fp,
            "node%u [label=\"node%u:\n\n%s\"];\n",
            n->page_number(),
            n->page_number(),
            to_string(n->entries()).c_str());
    for (uint32_t c : n->children())
        fprintf(fp, "    node%u -> node%u;\n", c, n->page_number());
    for (uint32_t c : n->children())
        write_dot_graph(retrieve_node(n->page_number(), c), fp);
}

template <class Callback>
void BtreeDirectory::recursive_iterate(const BtreeNode* n, const Callback& cb, int depth)
{
    dir_check(depth < BTREE_MAX_DEPTH);
    for (const Entry& e : n->entries())
        cb(e.filename, e.id, e.type);
    for (uint32_t c : n->children())
        recursive_iterate(retrieve_node(n->page_number(), c), cb, depth + 1);
}

void BtreeDirectory::iterate_over_entries(BtreeDirectory::callback cb)
{
    auto root = get_root_node();
    if (root)
        recursive_iterate(root, cb, 0);
}
}