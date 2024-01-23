#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include <dirent.h>
#include <liburing.h>

#include "../aws.h"
#include "../util.h"
#include "../debug.h"

#include <http_parser.h>
#include "../hashmap_s.h"

/**
 * Macros
*/
#define INITIAL_HASHMAP_SIZE 1024
#define MAX_CON_NO  4096
// io_uring
#define SUBMISSION_QUEUE_ENTRIES 2048
#define BUFFER_SIZE 8192

#define INITIAL_VECTOR_SIZE 8

/**
 * Structures and Enums
*/
enum IORequestType {
    NewConnection,
    ReadFromClient,
    OpenedLocalFile,
    LoadedFileToSend,
    SentToClient,
    ClosedClientSocket,
    ClosedOpenedFile,
};

struct Request {
    enum IORequestType event_type;

    int client_socket;
    int file_fd;

    int iovec_count;
    struct iovec iov[];
};

struct MappedFile {
    char* file_name;
    int fd;
    size_t file_size;
    void* address;
};

/**
 * Server Structure
*/
struct Server {
    int listening_socket;

    struct hashmap_s mapped_files;
    struct MappedFile** mfs_to_free;
    size_t mfs_size;
    size_t mfs_capacity;

    struct io_uring ring;
    int no_submitted_events;
    struct Request* accept_allocated_req;

    atomic_bool exit_server;
};

/**
 * Global variables
*/
struct Server inst; // single instance of the server

/**
 * Signal handlers
*/

static void sigint_handler(int /*signum*/)
{
    // printf("\nsigint\n");
    inst.exit_server = true;
}

/**
 * Utility functions
*/
// @note: The returned pointer needs to be deallocated
static char* get_united_string(const char* a, const char* b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    size_t len_new_s = len_a + len_b + 1;

    char* new_s = malloc(sizeof(*new_s) * len_new_s);
    DIE(new_s == NULL, "malloc failed in get_united_string");

    memcpy(new_s, a, len_a);
    memcpy(new_s + len_a, b, len_b + 1);

    return new_s;
}

static off_t get_file_size(const int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0) {
        dlog(LOG_WARNING, "Failed fstat in get_file_size\n");
        return -1;
    }

    if (S_ISBLK(st.st_mode)) {
        uint64_t bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            dlog(LOG_WARNING, "Failed ioctl call in get_file_size\n");
            return -1;
        }
        return bytes;
    } else if (S_ISREG(st.st_mode)) {
        return st.st_size;
    }

    return -1;
}

/**
 * Declarations
*/
static void initialize_server();

static int setup_listening_socket(unsigned short  port);
static void setup_io_uring(struct io_uring* ring, unsigned sub_queue_entries);
static void load_and_map_static_files();


static void start_server_loop();

static int add_accept_request(const int server_socket, struct sockaddr_in* client_addr, socklen_t* client_addr_len);
static int add_read_request(const int client_socket);
static int handle_client_request(struct Request* req);
static int http_req_on_path_cb(http_parser* p, const char* buf, size_t len);

static int http_response_fnf(struct Request* req);
static int http_response_with_file(struct Request* req);
static int http_response_with_mapped_file(struct Request* req, const char* path, size_t len);

static int open_file_to_load(struct Request* req, const char* path);
static int put_load_file_request(struct Request* req, const int fd);
static int put_close_file_descriptors_request(struct Request* req);

/**
 * Definitions
*/
void initialize_server()
{
    int res;

    // catch SIGINT (ctrl+C) signal
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    res = sigaction(SIGINT, &sa, NULL);
    DIE(res < 0, "sigaction SIGINT");

    inst.exit_server = false;
    inst.no_submitted_events = 0;

    // map static files into the memory
    load_and_map_static_files();

    inst.listening_socket = setup_listening_socket(AWS_LISTEN_PORT);

    setup_io_uring(&inst.ring, SUBMISSION_QUEUE_ENTRIES);
}

int setup_listening_socket(unsigned short port)
{
	struct sockaddr_in socket_address;
    int res;
    int socket_fd;
    int sock_opt;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    DIE(socket_fd < 0, "socket");

    sock_opt = 1;
    res = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &sock_opt, sizeof(sock_opt));
    DIE(res < 0, "setsockopt SO_REUSEADDR");

    sock_opt = 1;
    res = setsockopt(socket_fd, SOL_SOCKET, SO_ZEROCOPY, &sock_opt, sizeof(sock_opt));
    DIE(res < 0, "setsockopt SO_ZEROCOPY");

    memset(&socket_address, 0, sizeof(socket_address));
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(port);
    socket_address.sin_addr.s_addr = htonl(INADDR_ANY);

    res = bind(socket_fd, (const struct sockaddr*)&socket_address, sizeof(socket_address));
    DIE(res < 0, "bind");

    res = listen(socket_fd, MAX_CON_NO);
    DIE(res < 0, "listen");

    return socket_fd;
}

void setup_io_uring(struct io_uring* ring, unsigned sub_queue_entries)
{
    int res;

    res = io_uring_queue_init(sub_queue_entries, ring, 0);
    DIE(res != 0, "io_uring_queue_init_params");
}

void load_and_map_static_files()
{
    const char static_directory_path[] = AWS_ABS_STATIC_FOLDER;

    int res;

    res = hashmap_create(INITIAL_HASHMAP_SIZE, &inst.mapped_files);
    DIE(res != 0, "hashmap_create failed in load_and_map_static_files");

    // store the allocated structures in a separate vector for freeing later
    inst.mfs_size = 0;
    inst.mfs_capacity = INITIAL_VECTOR_SIZE;
    inst.mfs_to_free = malloc(inst.mfs_capacity * sizeof(*inst.mfs_to_free));
    DIE(inst.mfs_to_free == NULL, "malloc failed for inst.mfs_to_free in load_and_map_static_files");

    // get the files in the directory AWS_ABS_STATIC_FOLDER
    struct dirent* dp;
    DIR* dir = opendir(static_directory_path);
    DIE(dir == NULL, "opendir failed in load_and_map_static_files");

    // open them and store the name and fd in the hashmap
    while ((dp = readdir(dir)) != NULL) {
        if (dp->d_type != DT_REG) {
            continue;
        }

        // create a copy of the filename
        size_t name_size = strlen(dp->d_name) + 1;
        char* file_name = malloc(sizeof(char) * name_size);
        DIE(file_name == NULL, "malloc failed for file name in load_and_map_static_files");
        memcpy(file_name, dp->d_name, name_size);

        // open the file and map it
        char* path = get_united_string(AWS_ABS_STATIC_FOLDER, file_name);
        int fd = open(path, O_RDONLY);
        DIE(fd < 0, "open failed in load_and_map_static_files");
        free(path);

        off_t file_size = get_file_size(fd);

        void* address = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
        DIE(address == MAP_FAILED, "mmap failed in load_and_map_static_files");

        struct MappedFile* mf = malloc(sizeof(*mf));
        DIE(mf == NULL, "malloc failed for struct MappedFile in load_and_map_static_files");

        mf->file_name = file_name;
        mf->fd = fd;
        mf->file_size = file_size;
        mf->address = address;

        // insert in the hashmap
        res = hashmap_put(&inst.mapped_files, mf->file_name, strlen(file_name), mf);
        DIE(res != 0, "hashmap_put failed in load_and_map_static_files");

        // store the allocated memory also seperate for freeing later
        if (inst.mfs_size == inst.mfs_capacity) {
            inst.mfs_capacity *= 2;
            inst.mfs_to_free = realloc(inst.mfs_to_free, inst.mfs_capacity * sizeof(*inst.mfs_to_free));
            DIE(inst.mfs_to_free == NULL, "realloc for inst.mfs_to_free failed in load_and_map_static_files");
        }

        inst.mfs_to_free[inst.mfs_size] = mf;
        inst.mfs_size++;
    }

    closedir(dir);
}

void cleanup_server()
{
    int ret;
    struct io_uring_cqe* cqe;

    // that one is from the accept submitted event
    while (inst.no_submitted_events > 1) {
        ret = io_uring_wait_cqe(&inst.ring, &cqe);

        if (ret < 0) {
            dlog(LOG_DEBUG, "io_uring_wait_cqe in cleanup_server returned: %s (%d)\n", strerror(-ret), ret);
            continue;
        }

        inst.no_submitted_events--;
    }

    // clear also the allocated memory for the accept submitted event
    free(inst.accept_allocated_req);
    inst.accept_allocated_req = NULL;

    // free the io_uring structure
    io_uring_queue_exit(&inst.ring);

    // distroy the hashmap
    hashmap_destroy(&inst.mapped_files);

    // iterate through the mapped file structures and unmap the files and dealocate the memory
    for (size_t i = 0; i < inst.mfs_size; ++i) {
        struct MappedFile* mf = inst.mfs_to_free[i];

        ret = munmap(mf->address, mf->file_size);
        DIE(ret != 0, "munmap failed in cleanup_server");

        free(mf->file_name);
        free(mf);
    }

    free(inst.mfs_to_free);
}

void start_server_loop()
{
    int ret;
    struct io_uring_cqe* cqe;
    struct Request* req;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    add_accept_request(inst.listening_socket, &client_addr, &client_addr_len);
    struct __kernel_timespec time_to_wait = {
        .tv_sec = 0,
        .tv_nsec = 1000000, // 1ms
    };

    while (inst.exit_server == false) {
        ret = io_uring_wait_cqe_timeout(&inst.ring, &cqe, &time_to_wait);

        dlog(LOG_DEBUG, "we have an event!!!\n");

        if (ret == -EINTR || ret == -ETIME) {
            dlog(LOG_DEBUG, "io_uring_wait_cqe_timeout returned: %s (%d)\n", strerror(-ret), ret);
            continue;
        } else if (ret < 0) {
            dlog(LOG_WARNING, "io_uring_wait_cqe_timeout failed with: %s (%d)\n", strerror(-ret), ret);
            // check if file and socket descriptors are open
            continue;
        }

        inst.no_submitted_events--;

        req = (struct Request*)io_uring_cqe_get_data(cqe);
        if (cqe->res < 0) {
            dlog(LOG_WARNING, "Async request failed: %s for event: %d\n", strerror(-cqe->res), req->event_type);
            // check if file and socket descriptors are open
            if (req->client_socket != -1) {
                put_close_file_descriptors_request(req);
            }
            free(req);
            io_uring_cqe_seen(&inst.ring, cqe);
            continue;
        }

        switch (req->event_type) {
        case NewConnection:
            dlog(LOG_DEBUG, "New Connection Requested\n");
            // add a new accept request to the queue to accept new connections again
            add_accept_request(inst.listening_socket, &client_addr, &client_addr_len);
            add_read_request(cqe->res);
            free(req);
            break;

        case ReadFromClient:
            dlog(LOG_DEBUG, "Read from new client completed\n");
            // we read a new request from the client
            if (cqe->res == 0) {
                dlog(LOG_WARNING, "Empty request!\n");
                break;
            }
            handle_client_request(req);
            free(req->iov->iov_base);
            free(req);
            break;

        case OpenedLocalFile:
            dlog(LOG_DEBUG, "File opened and ready to be read\n");

            put_load_file_request(req, cqe->res);

            free(req);
            break;

        case LoadedFileToSend:
            dlog(LOG_DEBUG, "File loaded and ready to be sent to the client\n");

            http_response_with_file(req);

            free(req);
            break;

        case SentToClient:
            dlog(LOG_DEBUG, "Completed sending to the client the stuff\n");

            put_close_file_descriptors_request(req);

            // perform memory cleaning
            for (int i = 0; i < req->iovec_count; ++i) {
                free(req->iov[i].iov_base);
            }

            free(req);
            break;

        case ClosedClientSocket:
            dlog(LOG_DEBUG, "Closed client socket\n");
            free(req);
            break;

        case ClosedOpenedFile:
            dlog(LOG_DEBUG, "Closed opened file\n");
            free(req);
            break;

        default:
            dlog(LOG_DEBUG, "idk ce e: %d\n", req->event_type);
            break;
        }

        io_uring_cqe_seen(&inst.ring, cqe);
    }
}

int add_accept_request(const int server_socket, struct sockaddr_in* client_addr, socklen_t* client_addr_len)
{
    // get a submission queue event object to populate with our socket accept request
    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
    if (sqe == NULL) {
        dlog(LOG_WARNING, "could not obtain sqe for add_accept_request\n");
        return -1;
    }

    // object to use as private data to pass to the submission and get it back on the completion queue event
    struct Request* req = malloc(sizeof(*req));
    DIE(req == NULL, "malloc failed in add_accept_request");
    req->event_type = NewConnection;
    req->client_socket = -1;
    req->file_fd = -1;

    // prepare the accept submission queue event
    io_uring_prep_accept(sqe, server_socket, (struct sockaddr*)client_addr, client_addr_len, 0);

    // set the private data in the submission queue event
    io_uring_sqe_set_data(sqe, req);
    inst.accept_allocated_req = req;

    // anounce the API that the new submission queue event is ready
    // it takes only the io_uring ring as a parameter because internally it
    // already knows (I think, not 100% sure) that a submission queue event
    // can be submitted to the kernel
    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}

int add_read_request(const int client_socket)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
    if (sqe == NULL) {
        dlog(LOG_WARNING, "could not obtain sqe for add_read_request\n");
        return -1;
    }

    // prepare the private data to access on the completion queue event
    struct Request* req = malloc(sizeof(*req) + sizeof(struct iovec));
    DIE(req == NULL, "malloc failed in add_read_request");
    req->iovec_count = 1;
    req->iov[0].iov_base = malloc(BUFFER_SIZE);
    DIE(req->iov[0].iov_base == NULL, "malloc failed for iov in add_read_request");
    memset(req->iov[0].iov_base, 0, BUFFER_SIZE);
    req->iov[0].iov_len = BUFFER_SIZE;
    req->client_socket = client_socket;
    req->event_type = ReadFromClient;

    // prepare to read, hopefully it fits into just one window, if not, we'll see how to handle that
    io_uring_prep_readv(sqe, client_socket, &req->iov[0], 1, 0);

    // set the private data in the submission queue event object to access it later in the completed queue event object
    io_uring_sqe_set_data(sqe, req);

    // announce the API that a new submission is ready (behind the scenes knows how many to submit somehow)
    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}

int handle_client_request(struct Request* req)
{
    http_parser request_parser;
    static http_parser_settings settings_on_path = {
        /* on_message_begin */ 0,
        /* on_header_field */ 0,
        /* on_header_value */ 0,
        /* on_path */ http_req_on_path_cb,
        /* on_url */ 0,
        /* on_fragment */ 0,
        /* on_query_string */ 0,
        /* on_body */ 0,
        /* on_headers_complete */ 0,
        /* on_message_complete */ 0
    };

    // printf("HTTP message in:\n--->'%s'<---\n", (char*)req->iov->iov_base);

    http_parser_init(&request_parser, HTTP_REQUEST);
    request_parser.data = req;
    ((uint8_t*)req->iov->iov_base)[req->iov->iov_len-1] = '\0'; // make sure we are null terminated
    http_parser_execute(&request_parser, &settings_on_path, req->iov->iov_base, strlen(req->iov->iov_base));

    return 0;
}

int http_req_on_path_cb(http_parser* p, const char* buf, size_t len)
{
    int res = 0;
    struct Request* req = (struct Request*)p->data;

    // construct the path for the server
    size_t path_prefix = strlen(AWS_DOCUMENT_ROOT) - 1; // -1 to remove the extra '/'
    size_t path_length = path_prefix + len + 1; // +1 to have a '\0' at the end
    char* path = malloc(sizeof(*path) * path_length);
    DIE(path == NULL, "malloc failed in http_req_on_path_cb");
    memcpy(path, AWS_DOCUMENT_ROOT, path_prefix);
    memcpy(path + path_prefix, buf, len);
    path[path_prefix + len] = '\0';

    if(access(path, F_OK) == -1) {
        // check if the path exists and if not send reply with error 404 not found
        res = -1;
    } else if (strstr(path, AWS_REL_STATIC_FOLDER) != NULL) {
        // check if is a static file -> zero copy from already loaded files to socket
        res = http_response_with_mapped_file(req, path, path_length);
    } else if (strstr(path, AWS_REL_DYNAMIC_FOLDER) != NULL) {
        // check if is a dynamic file -> load file to program and zero copy to socket after
        res = open_file_to_load(req, path);
    }

    free(path);

    if (res != 0) {
        res = http_response_fnf(req);
    }

    return res;
}

// send file not found 404 HTTP response
int http_response_fnf(struct Request* req)
{
    printf("\t sending http 404\n");
    static const char fnf_response[] = "HTTP/1.1 404 Not Found\r\n"
		"\r\n"
        "404 Not Found";

    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
    if (sqe == NULL) {
        dlog(LOG_WARNING, "could not obtain sqe for http_response_fnf\n");
        return -1;
    }

    struct Request* new_req = malloc(sizeof(*new_req) + sizeof(struct iovec));
    DIE(new_req == NULL, "malloc failed in http_response_fnf");
    new_req->event_type = SentToClient;
    new_req->client_socket = req->client_socket;
    new_req->file_fd = -1;
    new_req->iovec_count = 0; // this means 0 of them need to be dealocated
    new_req->iov->iov_base = (void*)fnf_response;
    new_req->iov->iov_len = sizeof(fnf_response);
    // ok, order is important. We have to set the private data after preparing to writev
    io_uring_prep_writev(sqe, new_req->client_socket, new_req->iov, new_req->iovec_count, 0);
    io_uring_sqe_set_data(sqe, new_req);

    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}

// send file that is loaded in the req iovec data
int http_response_with_file(struct Request* req)
{
    static const char http_response_header[] = "HTTP/1.1 200 OK\r\n"
		"\r\n";

    int res;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
    if (sqe == NULL) {
        dlog(LOG_WARNING, "Could not obtain sqe for http_response_with_file\n");
        return -1;
    }

    int iovec_blocks = req->iovec_count + 1; // +1 for the first block being the http response

    struct Request* new_req = malloc(sizeof(*new_req) + sizeof(struct iovec) * iovec_blocks);
    DIE(new_req == NULL, "malloc failed for new_req in http_response_fnf");
    new_req->event_type = SentToClient;
    new_req->client_socket = req->client_socket;
    new_req->file_fd = req->file_fd;
    new_req->iovec_count = iovec_blocks;

    DIE(sizeof(http_response_header) > BUFFER_SIZE,
            "response header too big, need to allocate more for it in http_response_with_file\n");
    res = posix_memalign(&new_req->iov[0].iov_base, BUFFER_SIZE, BUFFER_SIZE);
    DIE(res != 0, "Failed posix_memalign in http_response_with_file\n");
    memcpy(new_req->iov[0].iov_base, http_response_header, sizeof(http_response_header));

    new_req->iov[0].iov_len = sizeof(http_response_header);
    for (int i = 1; i < iovec_blocks; ++i) {
        new_req->iov[i].iov_base = req->iov[i-1].iov_base;
        new_req->iov[i].iov_len = req->iov[i-1].iov_len;
    }

    io_uring_prep_writev(sqe, new_req->client_socket, new_req->iov, new_req->iovec_count, 0);
    io_uring_sqe_set_data(sqe, new_req);

    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}

int http_response_with_mapped_file(struct Request* req, const char* path, size_t len)
{
    // get just the file name
    int l = len - 1;
    while (l >= 0 && path[l] != '/') {
        l--;
    }
    l++;
    const char* file_name = &(path[l]);

    struct MappedFile* const mf = hashmap_get(&inst.mapped_files, file_name, strlen(file_name));
    if (mf == NULL) {
        dlog(LOG_WARNING, "File '%s' not found in the hashmap of mapped files but present in the path\n", file_name);
        return -1;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
    if (sqe == NULL) {
        dlog(LOG_WARNING, "Could not obtain sqe for http_response_with_mapped_file\n");
        return -1;
    }

    struct Request* new_req = malloc(sizeof(*new_req));
    DIE(new_req == NULL, "malloc failed for new_req in http_response_with_mapped_file");
    new_req->event_type = SentToClient;
    new_req->client_socket = req->client_socket;
    new_req->file_fd = -1;
    new_req->iovec_count = 0;

    io_uring_prep_send(sqe, new_req->client_socket, mf->address, mf->file_size, 0);
    // if the library supports, use io_uring_prep_send_zc for zero copy instead of io_uring_prep_send
    // io_uring_prep_send_zc(sqe, new_req->client_socket, mf->address, mf->file_size, MSG_WAITALL, 0);

    io_uring_sqe_set_data(sqe, new_req);

    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}


int open_file_to_load(struct Request* req, const char* path)
{
    struct Request* new_req = malloc(sizeof(*new_req));
    new_req->event_type = OpenedLocalFile;
    new_req->client_socket = req->client_socket;
    new_req->iovec_count = 0;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
    if (sqe == NULL) {
        dlog(LOG_WARNING, "could not obtain sqe for open_file_to_load\n");
        return -1;
    }

    io_uring_prep_openat(sqe, AT_FDCWD, path, O_RDONLY, 0);
    io_uring_sqe_set_data(sqe, new_req);

    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}

int put_load_file_request(struct Request* req, const int fd)
{
    int res;

    off_t file_size = get_file_size(fd);
    off_t bytes_remaining = file_size;
    int current_block = 0;
    int blocks_no = (int) file_size / BUFFER_SIZE;
    if (file_size % BUFFER_SIZE) blocks_no++;

    // prepare the request
    struct Request* new_req = malloc(sizeof(*new_req) + sizeof(struct iovec) * blocks_no);
    DIE(new_req == NULL, "Failed malloc for req in put_load_file_request\n");
    new_req->event_type = LoadedFileToSend;
    new_req->client_socket = req->client_socket;
    new_req->file_fd = fd;
    new_req->iovec_count = blocks_no;

    while (bytes_remaining) {
        off_t bytes_to_read = bytes_remaining;
        if (bytes_to_read > BUFFER_SIZE) bytes_remaining = BUFFER_SIZE;

        new_req->iov[current_block].iov_len = bytes_to_read;

        void* buf;
        res = posix_memalign(&buf, BUFFER_SIZE, BUFFER_SIZE);
        DIE(res != 0, "Failed posix_memalign in put_load_file_request\n");
        new_req->iov[current_block].iov_base = buf;

        current_block++;
        bytes_remaining -= bytes_to_read;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
    if (sqe == NULL) {
        dlog(LOG_WARNING, "Could not obtain sqe for put_load_file_request\n");
        return -1;
    }
    io_uring_prep_readv(sqe, fd, new_req->iov, new_req->iovec_count, 0);
    io_uring_sqe_set_data(sqe, new_req);

    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}

int put_close_file_descriptors_request(struct Request* req)
{
    // create a request to close the client connection (socket)
    {
        struct Request* new_req = malloc(sizeof(*new_req));
        DIE(new_req == NULL, "malloc failed for closing socket in put_close_file_descriptors_request");
        new_req->event_type = ClosedClientSocket;

        struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
        if (sqe == NULL) {
            dlog(LOG_WARNING, "Could not obtain sqe for closing socket in put_close_file_descriptors_request\n");
            return -1;
        }

        io_uring_prep_close(sqe, req->client_socket);
        io_uring_sqe_set_data(sqe, new_req);
    }

    // create a request to close the opened file if exists
    if (req->file_fd == -1) {
        io_uring_submit(&inst.ring);
        inst.no_submitted_events++;
        return 0;
    }

    {
        struct Request* new_req = malloc(sizeof(*new_req));
        DIE(new_req == NULL, "malloc failed for closing file in put_close_file_descriptors_request");
        new_req->event_type = ClosedOpenedFile;

        struct io_uring_sqe* sqe = io_uring_get_sqe(&inst.ring);
        if (sqe == NULL) {
            io_uring_submit(&inst.ring); // do not forget to submit the socket closing sqe
            inst.no_submitted_events++;
            dlog(LOG_WARNING, "Could not obtain sqe for closing file in put_close_file_descriptors_request\n");
            return -1;
        }

        io_uring_prep_close(sqe, req->file_fd);
        io_uring_sqe_set_data(sqe, new_req);
    }

    io_uring_submit(&inst.ring);
    inst.no_submitted_events++;

    return 0;
}

/**
 * Main function
*/

int main()
{
    printf("\nHello, the server is initializing!\n");

    initialize_server();

    printf("\nThe server is listening...\n");

    start_server_loop();

    printf("\nCleanning up and shutting down the server!\n");

    cleanup_server();

    printf("\nGoodbye!\n");

    return 0;
}
