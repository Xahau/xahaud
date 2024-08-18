#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <memory>
#include <algorithm>
namespace snug
{

    int compare_entries_reverse(const void* a, const void* b)
    {
        const uint64_t* a_key = static_cast<const uint64_t*>(a);
        const uint64_t* b_key = static_cast<const uint64_t*>(b);

        // Unrolled comparison of 4 uint64_t values (4 * 8 = 32 bytes)
        if (b_key[0] > a_key[0]) return 1;
        if (b_key[0] < a_key[0]) return -1;

        if (b_key[1] > a_key[1]) return 1;
        if (b_key[1] < a_key[1]) return -1;

        if (b_key[2] > a_key[2]) return 1;
        if (b_key[2] < a_key[2]) return -1;

        if (b_key[3] > a_key[3]) return 1;
        if (b_key[3] < a_key[3]) return -1;

        return 0;  // Keys are equal
    }


    class SnugDB
    {

        private:
            static constexpr uint64_t SNUGSIZE = 256ull*1024ull*1024ull*1024ull;        // 256 GiB
            static constexpr uint64_t BIGSIZE = 10ull*1024ull*1024ull*1024ull*1024ull;  // 10 TiB


            static constexpr size_t BUCKET_COUNT = 1048576;

            std::unique_ptr<std::shared_mutex[]> mutexes = 
                std::make_unique<std::shared_mutex[]>(BUCKET_COUNT);


            // each file snug.0 snug.1 ... is mmaped and the pointer
            uint8_t* mapped_files[1024];
            uint64_t mapped_files_count { 0 };

            uint8_t* big_file; // this file has 64kib blocks in it which are used 
                               // as an overflow for large blobs

            std::mutex big_file_mutex; // locked when incrementing the "next new block" pointer

            // only used when adding a new file
            std::mutex mapped_files_count_mutex;

            std::string const path;

            // 0 = success
            // 1 = could not open
            // 2 = could not seek
            // 3 = could not write at end of file
            int alloc_file(char const* fn, uint64_t size)
            {
                int fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0)
                    return 1;

                // must be a multiple of bufsize

                if (lseek(fd, size, SEEK_SET) == -1)
                {
                    close(fd);
                    unlink(fn);
                    return 2;
                }

                if (write(fd, "", 1) != 1)
                {
                    close(fd);
                    unlink(fn);
                    return 3;
                }

                close(fd);
                return 0;
            }

            // 0 = file exists and is right size
            int check_file(char const* fn, uint64_t size)
            {
                struct stat st;
                int file_exists = (stat(fn, &st) == 0);

                if (!file_exists)
                    return 1;

                if (st.st_size != size + 1)
                    return 2;

                return 0;
            }

#define OFFSET(byte0, byte1, byte2)\
            (((((uint64_t)(byte0 & 0xFFU)) << 12) +\
              (((uint64_t)(byte1 & 0xFFU)) << 4) +\
              ((uint64_t)(byte2 & 0xFU))) << 18)


            // check if 32 bytes are 0, which they will be for a zero entry
#define IS_ZERO_ENTRY(x)\
            (*((uint64_t*)((x)+ 0)) == 0 && \
             *((uint64_t*)((x)+ 8)) == 0 && \
             *((uint64_t*)((x)+16)) == 0 && \
             *((uint64_t*)((x)+24)) == 0)

#define IS_ENTRY(x,y)\
            (*((uint64_t*)((x)+ 0)) == *((uint64_t*)((y)+ 0)) && \
             *((uint64_t*)((x)+ 8)) == *((uint64_t*)((y)+ 8)) && \
             *((uint64_t*)((x)+16)) == *((uint64_t*)((y)+16)) && \
             *((uint64_t*)((x)+24)) == *((uint64_t*)((y)+24)))

#define WRITE_KEY(x /* dst */, y /* src */, flags)\
            {\
                *((uint64_t*)((x)+ 0)) = *((uint64_t*)((y)+ 0)); \
                *((uint64_t*)((x)+ 8)) = *((uint64_t*)((y)+ 8)); \
                *((uint64_t*)((x)+16)) = *((uint64_t*)((y)+16)); \
                *((uint64_t*)((x)+24)) = *((uint64_t*)((y)+24)); \
                *((uint64_t*)((x)+32)) = flags;\
            }

            // if an entry exceeds 984 bytes then the overflow is written
            // into the snug.big file in a linked list of 32kib blocks
            // the first of those blocks is a control block

            uint64_t get_big_block()
            {
                std::unique_lock<std::mutex> lock(big_file_mutex);

                uint64_t free_blocks = *((uint64_t*)(big_file + 8));
                if (free_blocks == 0)
                {
                    // no free blocks, allocate a new one
                    uint64_t next_block = *((uint64_t*)big_file);
                    *((uint64_t*)(big_file)) += 32768;

                    if (next_block + 32768 > BIGSIZE)
                        return 0;

                    return next_block;
                }

                // grab the nth one
                uint8_t* offset = big_file + 16 
                    + 8 * (free_blocks - 1);

                // decrement free block counter
                *(uint64_t*)(big_file + 8) -= 1;

                return *((uint64_t*)offset);
            }

            void unalloc_blocks(uint64_t next_block)
            {
                if (next_block != 0)
                {
                    // scope the lock only if called with non-zero nextblock
                    std::unique_lock<std::mutex> lock(big_file_mutex);
                    do
                    {
                        uint64_t free_blocks = *((uint64_t*)(big_file + 8));

                        if (free_blocks >= 4095)
                            break;


                        uint8_t* offset = big_file + 16 
                            + 8 * free_blocks;

                        *((uint64_t*) offset) = next_block;

                        *((uint64_t*)(big_file + 8)) += 1;

                        uint8_t* big_ptr = big_file + next_block;
                        uint64_t previous = next_block;
                        next_block = *((uint64_t*)(big_file + next_block));

                        // clear the pointer on the old block
                        *((uint64_t*)(big_file + previous)) = 0;
                    }
                    while (next_block != 0);
                }
            }

            /*
             * First big entry is control block:
             * 0 - 7: The next free new block
             * 8 - 15: The number of free blocks blow
             * 16 - 23 [... repeating]: The next free unused block
             */
            /*
             * Big entry format:
             * 0   -     7: next block in chain, if any.
             * 8  - 32767: payload
             */

            // return 0 = failure
            //        > 0 = first block in the chain
            uint64_t write_big_entry_internal(uint8_t* data, ssize_t len, uint64_t next_block)
            { 

                uint64_t first_block = 0;

                uint64_t* last_block_ptr = 0;
                do
                {
                    // if next_block is populated we follow an existing pathway
                    // otherwise allocate a new block now

                    if (!next_block)
                        next_block = get_big_block();

                    if (!next_block)
                        return 0;

                    if (!first_block)
                        first_block = next_block;

                    if (last_block_ptr)
                        *last_block_ptr = next_block;

                    uint8_t* big_ptr = big_file + next_block;

                    // copy to the block
                    ssize_t to_write = len > 32760 ? 32760 : len;
                    memcpy(big_ptr + 8, data, to_write);

                    data += to_write;
                    len -= to_write;

                    next_block = *((uint64_t*)big_ptr);
                    last_block_ptr = (uint64_t*)big_ptr;
                }
                while (len > 0);

                // if there's a dangling chain we'll unallocate it 
                if (next_block != 0)
                    unalloc_blocks(next_block);

                return first_block;
            }

            /*
             * Entry format:
             * 0    - 31: the 32 byte key
             * 32   - 39: flags (high 4 bytes are flags, low 4 are size)
             * 40 - 1023: data (up to 984 bytes)
             */
            // 0 = success
            // 1 = bucket full
            // 2 = big blocks full
            int write_entry_internal(uint8_t* data, uint8_t* key, uint8_t* val, uint32_t len)
            {
                // find the entry
                uint64_t offset = OFFSET(key[0], key[1], (key[2]>>4));

                // lock the bucket for writing
                std::unique_lock<std::shared_mutex> lock(mutexes[offset >> 18]);

                uint8_t* start = data + offset;
                for (int i = 0; i < 256*1024; i+=1024)
                {
                    if (!IS_ENTRY(start + i, key) && !IS_ZERO_ENTRY(start + i))
                        continue;    


                    // read flags
                    uint64_t flags = *((uint64_t*)(start + i + 32));


                    // big entries are tricky
                    bool const old_big = (flags >> 32) != 0;
                    bool const new_big = len > 984;

                    if (new_big)
                    {
                        //write_big_entry_internal(uint8_t* data, ssize_t len, uint64_t next_block)
                        uint64_t first_block = 
                            write_big_entry_internal(val + 984, len - 984, (old_big ? (flags >> 32) : 0));

                        if (first_block == 0) // error state
                        {
                            if (old_big)
                                unalloc_blocks(flags >> 32);

                            return 2;
                        }

                        flags = (first_block << 32) + len;
                    }
                    else if (old_big)   // big blocks exist but new value is small
                    {
                        // unallocate the old chain
                        unalloc_blocks(flags >> 32);
                    }

                    if (!new_big)
                        flags = len;

                    /// write entry
                    WRITE_KEY(start + i, key, flags);
                    memcpy(start + i + 40, val, (len > 984 ? 984 : len));

                    // sort the bucket backwards so 0's appear at the end
                    qsort(start, 256, 1024, compare_entries_reverse);

                    return 0;
                }

                /// file (bucket) full
                return 1;
            }

            // out_len carries the length of the output buffer when calling and is replaced
            // with the length of the data found when returning
            int read_entry_internal(uint8_t* data, uint8_t* key, uint8_t* val_out, uint64_t* out_len)
            {
                uint64_t buf_len = *out_len;

                // find the entry
                uint64_t offset = OFFSET(key[0], key[1], (key[2]>>4));
                uint8_t* start = data + offset;

                // lock the bucket for reading
                std::shared_lock<std::shared_mutex> lock(mutexes[offset >> 18]);

                for (int i = 0; i < 256*1024; i+=1024)
                {
                    if (IS_ZERO_ENTRY(start + i))
                        return 1;

                    if (!IS_ENTRY(start + i, key))
                        continue;

                    // read out the value

                    uint64_t flags = *((uint64_t*)(start + i + 32));

                    uint32_t size = flags & 0xFFFFFFFFUL;
                    uint64_t next_block = flags >> 32;

                    if (size > buf_len)
                        return 2;

                    *out_len = size;

                    size_t to_read = size > 984 ? 984: size;
                    memcpy(val_out, start + i + 40, to_read);

                    val_out += to_read;
                    size -= to_read;

                    // big block read logic
                    while (size > 0)
                    {
                        // follow big block pointers
                        if (!next_block)
                        {
                            printf("End while size=%d\n", size);
                            return 3;
                        }

                        uint8_t* big_ptr = big_file + next_block;
                        to_read =  size > 32760 ? 32760 : size;
                        memcpy(val_out, big_ptr + 8, to_read);

                        val_out += to_read;
                        size -= to_read;

                        next_block = *((uint64_t*)big_ptr);
                    }

                    return 0;
                }

                return 1;
            }

            void setup()
            {
                struct stat path_stat;

                if (stat(path.c_str(), &path_stat) != 0)
                    throw std::runtime_error("Error checking path: " + path + " - " + std::string(strerror(errno)));

                if (!S_ISDIR(path_stat.st_mode))
                    throw std::runtime_error("Path is not a directory: " + path);

                if (access(path.c_str(), R_OK | W_OK | X_OK) != 0)
                    throw std::runtime_error("Insufficient permissions for path: " + path);

                // Search for existing snug files sequentially
                std::vector<std::string> snug_files;
                for (int file_index = 0; file_index < 1024; ++file_index)
                {
                    std::string filename = "snug." + std::to_string(file_index);
                    std::string full_path = path + "/" + filename;

                    if (access(full_path.c_str(), F_OK) != 0)
                        break;

                    snug_files.push_back(filename);
                }

                // If no files found, create snug.0
                if (snug_files.empty())
                {
                    std::string new_file = path + "/snug.0";
                    int result = alloc_file(new_file.c_str(), SNUGSIZE);
                    if (result != 0)
                        throw std::runtime_error("Failed to create initial file: " + new_file);
                    snug_files.push_back("snug.0");
                }

                // Memory map all files
                for (const auto& file : snug_files)
                {
                    std::string full_path = path + "/" + file;
                    if (check_file(full_path.c_str(), SNUGSIZE) != 0)
                        throw std::runtime_error("File was the wrong size: " + file);

                    int fd = open(full_path.c_str(), O_RDWR);
                    if (fd == -1)
                        throw std::runtime_error("Unable to open file: " + full_path);

                    struct stat file_stat;
                    if (fstat(fd, &file_stat) == -1)
                    {
                        close(fd);
                        throw std::runtime_error("Unable to get file stats: " + full_path);
                    }

                    void* mapped = mmap(nullptr, file_stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    close(fd);  // Can close fd after mmap

                    if (mapped == MAP_FAILED)
                        throw std::runtime_error("Unable to mmap file: " + full_path);

                    mapped_files[mapped_files_count++] = static_cast<uint8_t*>(mapped);
                }

                // create and map snug.big overflow file
                {
                    std::string new_file = path + "/snug.big";
                    if (check_file(new_file.c_str(), BIGSIZE) != 0)
                    {
                        int result = alloc_file(new_file.c_str(), BIGSIZE);
                        if (result != 0)
                            throw std::runtime_error("Failed to create initial file: " + new_file);
                    }

                    int fd = open(new_file.c_str(), O_RDWR);
                    if (fd == -1)
                        throw std::runtime_error("Unable to open file: " + new_file);

                    struct stat file_stat;
                    if (fstat(fd, &file_stat) == -1)
                    {
                        close(fd);
                        throw std::runtime_error("Unable to get file stats: " + new_file);
                    }

                    void* mapped = mmap(nullptr, file_stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    close(fd);  // Can close fd after mmap

                    if (mapped == MAP_FAILED)
                        throw std::runtime_error("Unable to mmap file: " + new_file);

                    big_file = static_cast<uint8_t*>(mapped);
                }
            }
        public:

            SnugDB(std::string path_) : path(path_)
        {
            setup();
        }

            ~SnugDB()
            {
                // Unmap all files in destructor
                // RH TODO: consider lock here
                for (int i = 0; i < mapped_files_count; ++i)
                    munmap(mapped_files[i], SNUGSIZE);

                // unmap the big file
                munmap(big_file, BIGSIZE);
            }

            int write_entry(uint8_t* key, uint8_t* val, ssize_t len)
            {
                for (size_t i = 0; i < mapped_files_count; ++i)
                {
                    int result = write_entry_internal(mapped_files[i], key, val, len);
                    if (result == 0)
                        return 0;

                    if (result != 1) // only bucket full falls through
                        return result;
                }

                // All existing files are full, allocate a new one
                {
                    // acquire the mutex
                    const std::lock_guard<std::mutex> lock(mapped_files_count_mutex);

                    std::string new_file = path + "/snug." + std::to_string(mapped_files_count);
                    int alloc_result = alloc_file(new_file.c_str(), SNUGSIZE);
                    if (alloc_result != 0)
                        return alloc_result + 10;  // Return error code from alloc_file if it fails (+10)

                    int fd = open(new_file.c_str(), O_RDWR);
                    if (fd == -1)
                        return 1;  // Return 1 for open failure

                    struct stat file_stat;
                    if (fstat(fd, &file_stat) == -1)
                    {
                        close(fd);
                        return 2;  // Return 2 for fstat failure
                    }

                    void* mapped = mmap(nullptr, file_stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    close(fd);  // Can close fd after mmap

                    if (mapped == MAP_FAILED)
                        return 3;  // Return 3 for mmap failure

                    // add the new file to the map, and increment the counter
                    mapped_files[mapped_files_count] = static_cast<uint8_t*>(mapped);

                    // this is the last possible thing done
                    mapped_files_count++;
                }

                // finally write the entry
                // RH TODO: consider adding a recursion guard here
                return write_entry(key, val, len);
            }

            int read_entry(uint8_t* key, uint8_t* val_out, uint64_t* out_len_orig)
            {

                for (size_t i = 0; i < mapped_files_count; ++i)
                {
                    uint64_t out_len = *out_len_orig;

                    int result =
                        read_entry_internal(mapped_files[i], key, val_out, &out_len);

                    if (result == 0)
                    {
                        *out_len_orig = out_len;
                        return 0;  // Entry found and read successfully
                    }

                    if (result == 2)
                        return 2;  // Output buffer too small
                }

                // Entry not found in any file
                return 1;
            }


            void visit_all(void (*f)(uint8_t*, uint8_t*, uint64_t, void* /*opaque caller val*/),
                    void* opaque)
            {
                // to visit all we only need to check snug.0 to begin with
                // we go to the first bucket
                // if we find no entries there we go to the next bucket
                // if we find entries there then we need to count them,
                // if we find 256 entries there then we go to snug.1 and so on until we run out
                // we merge sort the entries into a list for the visit

                for (uint64_t bucket = 0; bucket < BUCKET_COUNT; ++bucket)
                {
                    // acquire the bucket lock
                    std::shared_lock<std::shared_mutex> lock(mutexes[bucket]);

                    // check the bucket
                    uint8_t* ptr = mapped_files[0] + (bucket << 18);

                    if (*((uint64_t*)(ptr + 32)) == 0)
                        continue;

                    //if (IS_ZERO_ENTRY(ptr))
                    //    continue;

                    // live bucket, collect entries
                    std::vector<uint8_t*> entries;
                    {
                        // need to acquire the mutex to prevent a race condition
                        // where a new file is being added while we're searching
                        const std::lock_guard<std::mutex> lock(mapped_files_count_mutex);

                        // preallocate worst case scenario, RIP memory
                        entries.reserve(mapped_files_count * 256);

                        for (int i = 0; i < mapped_files_count; ++i)
                        {
                            uint8_t* ptr = mapped_files[i] + (bucket << 18);
                            for (int entry_count = 0;
                                    !IS_ZERO_ENTRY(ptr) && entry_count < 256;
                                    ++entry_count, ptr += 1024)
                                entries.push_back(ptr);
                        }
                    }

                    if (entries.empty())
                        continue;

                    // sort the entries
                    std::sort(entries.begin(), entries.end(),
                        [](const uint8_t* a, const uint8_t* b)
                        {
                            return memcmp(a, b, 32) < 0;
                        });

                    for (auto e : entries)
                    {
                        // visitation
                        uint8_t* entry = &e[0];
                        uint64_t flags = *((uint64_t*)(entry + 32));
                        uint64_t next_block = flags >> 32;
                        uint64_t size = flags & 0xFFFFFFFFULL;

                        if (size <= 984)
                        {
                            f(entry, entry + 40, size, opaque);
                            continue;
                        }

                        // copy big entry to a buffer
                        std::unique_ptr<uint8_t[]> copybuf = std::make_unique<uint8_t[]>(size);

                        uint8_t* data = &(copybuf[0]);
                        memcpy(data, entry + 40, 984);

                        data += 984;
                        size -= 984;
                    

                        // big block read logic
                        while (size > 0)
                        {
                            // follow big block pointers
                            if (!next_block)
                            {
                                printf("End while size=%lu\n", size);
                                return;
                            }

                            uint8_t* big_ptr = big_file + next_block;
                            uint64_t to_read =  size > 32760 ? 32760 : size;
                            memcpy(data, big_ptr + 8, to_read);

                            data += to_read;
                            size -= to_read;

                            next_block = *((uint64_t*)big_ptr);
                        }

                        f(entry, data, (flags & 0xFFFFFFFFULL), opaque);
                    }
                }

            }

    };

}
