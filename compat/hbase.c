#include "hbase.h"

#ifdef OS_DARWIN
#include <mach-o/dyld.h> // for _NSGetExecutablePath
#endif

#include "hatomic.h"

#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

static hatomic_t s_alloc_cnt = HATOMIC_VAR_INIT(0);
static hatomic_t s_free_cnt = HATOMIC_VAR_INIT(0);

long hv_alloc_cnt() {
    return s_alloc_cnt;
}

long hv_free_cnt() {
    return s_free_cnt;
}

void* hv_malloc(size_t size) {
    void* ptr;
    hatomic_inc(&s_alloc_cnt);
    ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "malloc failed!\n");
        exit(-1);
    }
    return ptr;
}

void* hv_realloc(void* oldptr, size_t newsize, size_t oldsize) {
    void *ptr;
    hatomic_inc(&s_alloc_cnt);
    if (oldptr) hatomic_inc(&s_free_cnt);
    ptr = realloc(oldptr, newsize);
    if (!ptr) {
        fprintf(stderr, "realloc failed!\n");
        exit(-1);
    }
    if (newsize > oldsize) {
        memset((char*)ptr + oldsize, 0, newsize - oldsize);
    }
    return ptr;
}

void* hv_calloc(size_t nmemb, size_t size) {
    void *ptr;
    hatomic_inc(&s_alloc_cnt);
    ptr =  calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "calloc failed!\n");
        exit(-1);
    }
    return ptr;
}

void* hv_zalloc(size_t size) {
    void* ptr;
    hatomic_inc(&s_alloc_cnt);
    ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "malloc failed!\n");
        exit(-1);
    }
    memset(ptr, 0, size);
    return ptr;
}

void hv_free(void* ptr) {
    if (ptr) {
        free(ptr);
        ptr = NULL;
        hatomic_inc(&s_free_cnt);
    }
}

char* hv_strupper(char* str) {
    char* p = str;
    while (*p != '\0') {
        if (*p >= 'a' && *p <= 'z') {
            *p &= ~0x20;
        }
        ++p;
    }
    return str;
}

char* hv_strlower(char* str) {
    char* p = str;
    while (*p != '\0') {
        if (*p >= 'A' && *p <= 'Z') {
            *p |= 0x20;
        }
        ++p;
    }
    return str;
}

char* hv_strreverse(char* str) {
    char *b, *e, tmp;
    if (str == NULL) return NULL;
    b = e = str;
    while(*e) {++e;}
    --e;
    while (e > b) {
        tmp = *e;
        *e = *b;
        *b = tmp;
        --e;
        ++b;
    }
    return str;
}

// n = sizeof(dest_buf)
char* hv_strncpy(char* dest, const char* src, size_t n) {
    char *ret;
    assert(dest != NULL && src != NULL);
    ret = dest;
    while (*src != '\0' && --n > 0) {
        *dest++ = *src++;
    }
    *dest = '\0';
    return ret;
}

// n = sizeof(dest_buf)
char* hv_strncat(char* dest, const char* src, size_t n) {
    char *ret;
    assert(dest != NULL && src != NULL);
    ret = dest;
    while (*dest) {++dest;--n;}
    while (*src != '\0' && --n > 0) {
        *dest++ = *src++;
    }
    *dest = '\0';
    return ret;
}

char* hv_strdup(const char* s) {
    int len = strlen(s);
    char* p = malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}

#if defined(MSVC_VERSION) &&  MSVC_VERSION<=71
size_t strnlen(const char *s, size_t maxlen)
{
    size_t i = 0;
    while (s[i++] != '\0' && (i < maxlen))
        ;
    return i;
}
#endif

char* hv_strndup(const char* s, size_t n) {
    size_t len = strnlen(s, n);
    char* p = malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}

char *hv_strcasestr(const char *haystack, const char *needle)
{
#if _GNU_SOURCE && !defined(COMPILER_MINGW)
    return strcasestr(haystack, needle);
#else
    const char *p;
    if (!*needle) {
        return (char*)haystack;
    }
    for (p = haystack; *p != '\0'; ++p) {
        const char *h = p, *n = needle;
        while (tolower((unsigned char)*h) == tolower((unsigned char)*n) && *n != '\0') {
            h++;
            n++;
        }
        if (!*n) {
            return (char*)p;
        }
        if (tolower((unsigned char)*p) != tolower((unsigned char)*needle)) {
            while (*p && tolower((unsigned char)*p) != tolower((unsigned char)*needle)) {
                p++;
            }
        }
        if (!*p) {
            break;
        }
    }
    return NULL;
#endif
}

bool hv_strstartswith(const char* str, const char* start) {
    assert(str != NULL && start != NULL);
    while (*str && *start && *str == *start) {
        ++str;
        ++start;
    }
    return *start == '\0';
}

bool hv_strendswith(const char* str, const char* end) {
    int len1 = 0;
    int len2 = 0;
    assert(str != NULL && end != NULL);
    while (*str) {++str; ++len1;}
    while (*end) {++end; ++len2;}
    if (len1 < len2) return false;
    while (len2-- > 0) {
        --str;
        --end;
        if (*str != *end) {
            return false;
        }
    }
    return true;
}

bool hv_strcontains(const char* str, const char* sub) {
    assert(str != NULL && sub != NULL);
    return strstr(str, sub) != NULL;
}

bool hv_wildcard_match(const char* str, const char* pattern) {
    bool match = false;
    assert(str != NULL && pattern != NULL);
    while (*str && *pattern) {
        if (*pattern == '*') {
            match = hv_strendswith(str, pattern + 1);
            break;
        } else if (*str != *pattern) {
            match = false;
            break;
        } else {
            ++str;
            ++pattern;
        }
    }
    return match ? match : (*str == '\0' && *pattern == '\0');
}

char* hv_strnchr(const char* s, char c, size_t n) {
    const char* p = s;
    assert(s != NULL);
    while (*p != '\0' && n-- > 0) {
        if (*p == c) return (char*)p;
        ++p;
    }
    return NULL;
}

char* hv_strrchr_dir(const char* filepath) {
    char* p = (char*)filepath;
    while (*p) ++p;
    while (--p >= filepath) {
#ifdef OS_WIN
        if (*p == '/' || *p == '\\')
#else
        if (*p == '/')
#endif
            return p;
    }
    return NULL;
}

const char* hv_basename(const char* filepath) {
    const char* pos = hv_strrchr_dir(filepath);
    return pos ? pos+1 : filepath;
}

const char* hv_suffixname(const char* filename) {
    const char* pos = hv_strrchr_dot(filename);
    return pos ? pos+1 : "";
}

int hv_mkdir_p(const char* dir) {
    if (access(dir, 0) == 0) {
        return EEXIST;
    }else{
        char *tmp = strdup(dir);
        char* p = tmp;
        char delim = '/';
        while (*p) {
#ifdef OS_WIN
            if (*p == '/' || *p == '\\') {
                delim = *p;
#else
            if (*p == '/') {
#endif
                *p = '\0';
                hv_mkdir(tmp);
                *p = delim;
            }
            ++p;
        }
        if (hv_mkdir(tmp) != 0) {
            return EPERM;
        }
        free(tmp);
    }
    return 0;
}

int hv_rmdir_p(const char* dir) {
    if (access(dir, 0) != 0) {
        return ENOENT;
    }
    if (rmdir(dir) != 0) {
        return EPERM;
    } else {
        char *tmp, *p;
        p = tmp = strdup(dir);
        while (*p) ++p;
        while (--p >= tmp) {
    #ifdef OS_WIN
            if (*p == '/' || *p == '\\') {
    #else
            if (*p == '/') {
    #endif
                *p = '\0';
                if (rmdir(tmp) != 0) {
                    return 0;
                }
            }
        }
        free(tmp);
    }
    return 0;
}

bool hv_exists(const char* path) {
    return access(path, 0) == 0;
}

bool hv_isdir(const char* path) {
    struct stat st;
    if (access(path, 0) != 0) return false;
    memset(&st, 0, sizeof(st));
    stat(path, &st);
    return S_ISDIR(st.st_mode);
}

bool hv_isfile(const char* path) {
    struct stat st;
    if (access(path, 0) != 0) return false;
    memset(&st, 0, sizeof(st));
    stat(path, &st);
    return S_ISREG(st.st_mode);
}

bool hv_islink(const char* path) {
#ifdef OS_WIN
    return hv_isdir(path) && (GetFileAttributes(path) & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    if (access(path, 0) != 0) return false;
    struct stat st;
    memset(&st, 0, sizeof(st));
    lstat(path, &st);
    return S_ISLNK(st.st_mode);
#endif
}

size_t hv_filesize(const char* filepath) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    stat(filepath, &st);
    return st.st_size;
}

char* get_executable_path(char* buf, int size) {
#ifdef OS_WIN
    GetModuleFileName(NULL, buf, size);
#elif defined(OS_LINUX)
    if (readlink("/proc/self/exe", buf, size) == -1) {
        return NULL;
    }
#elif defined(OS_DARWIN)
    _NSGetExecutablePath(buf, (uint32_t*)&size);
#endif
    return buf;
}

char* get_executable_dir(char* buf, int size) {
    char filepath[MAX_PATH] = {0};
    char* pos;
    get_executable_path(filepath, sizeof(filepath));
    pos = hv_strrchr_dir(filepath);
    if (pos) {
        *pos = '\0';
        strncpy(buf, filepath, size);
    }
    return buf;
}

char* get_executable_file(char* buf, int size) {
    char filepath[MAX_PATH] = {0};
    char* pos;
    get_executable_path(filepath, sizeof(filepath));
    pos = hv_strrchr_dir(filepath);
    if (pos) {
        strncpy(buf, pos+1, size);
    }
    return buf;
}

char* get_run_dir(char* buf, int size) {
    return getcwd(buf, size);
}

int hv_rand(int min, int max) {
    static int s_seed = 0;
    int _rand;
    assert(max > min);

    if (s_seed == 0) {
        s_seed = time(NULL);
        srand(s_seed);
    }

    _rand = rand();
    _rand = min + (int) ((double) ((double) (max) - (min) + 1.0) * ((_rand) / ((RAND_MAX) + 1.0)));
    return _rand;
}

char* hv_random_string(char *buf, int len) {
    static char s_characters[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U',
        'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
        'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };
    int i = 0;
    for (; i < len; i++) {
        buf[i] = s_characters[hv_rand(0, sizeof(s_characters) - 1)];
    }
    buf[i] = '\0';
    return buf;
}

bool hv_getboolean(const char* str) {
    int len;
    if (str == NULL) return false;
    len = strlen(str);
    if (len == 0) return false;
    switch (len) {
    case 1: return *str == '1' || *str == 'y' || *str == 'Y';
    case 2: return stricmp(str, "on") == 0;
    case 3: return stricmp(str, "yes") == 0;
    case 4: return stricmp(str, "true") == 0;
    case 6: return stricmp(str, "enable") == 0;
    default: return false;
    }
}

uint64_t hv_parse_size(const char* str) {
    uint64_t size = 0, n = 0;
    const char* p = str;
    char c;
    while ((c = *p) != '\0') {
        if (c >= '0' && c <= '9') {
            n = n * 10 + c - '0';
        } else {
            switch (c) {
            case 'K': case 'k': n <<= 10; break;
            case 'M': case 'm': n <<= 20; break;
            case 'G': case 'g': n <<= 30; break;
            case 'T': case 't': if(sizeof(size_t) > 5) n <<= 40; break;
            default:                      break;
            }
            size += n;
            n = 0;
        }
        ++p;
    }
    return size + n;
}

time_t hv_parse_time(const char* str) {
    time_t time = 0, n = 0;
    const char* p = str;
    char c;
    while ((c = *p) != '\0') {
        if (c >= '0' && c <= '9') {
            n = n * 10 + c - '0';
        } else {
            switch (c) {
            case 's':                           break;
            case 'm': n *= 60;                  break;
            case 'h': n *= 60 * 60;             break;
            case 'd': n *= 24 * 60 * 60;        break;
            case 'w': n *= 7 * 24 * 60 * 60;    break;
            default:                            break;
            }
            time += n;
            n = 0;
        }
        ++p;
    }
    return time + n;
}

int hv_parse_url(hurl_t* stURL, const char* strURL) {
    const char* begin = strURL;
    const char* end = strURL;
    if (stURL == NULL || strURL == NULL) return -1;
    memset(stURL, 0, sizeof(hurl_t));
    while (*end != '\0') ++end;
    if (end - begin > 65535) return -2;
    // scheme://
    {
        const char* sp = strURL;
        const char* ep = strstr(sp, "://");
        const char *user, *host, *pos, *port, *hostend;
        if (ep) {
            // stURL->fields[HV_URL_SCHEME].off = sp - begin;
            stURL->fields[HV_URL_SCHEME].len = ep - sp;
            sp = ep + 3;
        }
        // user:pswd@host:port
        ep = strchr(sp, '/');
        if (ep == NULL) ep = end;
        user = sp;
        host = sp;
        pos = hv_strnchr(sp, '@', ep - sp);
        if (pos) {
            // user:pswd
            const char* pswd = hv_strnchr(user, ':', pos - user);
            if (pswd) {
                stURL->fields[HV_URL_PASSWORD].off = pswd + 1 - begin;
                stURL->fields[HV_URL_PASSWORD].len = pos - pswd - 1;
            } else {
                pswd = pos;
            }
            stURL->fields[HV_URL_USERNAME].off = user - begin;
            stURL->fields[HV_URL_USERNAME].len = pswd - user;
            // @
            host = pos + 1;
        }
        // host:port or ipv4:port or [ipv6]:port
        hostend = host;
        if (*host == '[') {
            pos = hv_strnchr(host, ']', ep - host);
            if (pos) {
                // ipv6
                host++;
                hostend = pos;
                stURL->fields[HV_URL_HOST].off = host - begin;
                stURL->fields[HV_URL_HOST].len = hostend - host;
            }
        }
        port = hv_strnchr(hostend, ':', ep - hostend);
        if (port) {
            stURL->fields[HV_URL_PORT].off = port + 1 - begin;
            stURL->fields[HV_URL_PORT].len = ep - port - 1;
            // atoi
            {
                unsigned short i = 1;
                for (; i <= stURL->fields[HV_URL_PORT].len; ++i) {
                    stURL->port = stURL->port * 10 + (port[i] - '0');
                }
            }
        } else {
            port = ep;
            // set default port
            stURL->port = 80;
            if (stURL->fields[HV_URL_SCHEME].len > 0) {
                if (strncmp(strURL, "https://", 8) == 0) {
                    stURL->port = 443;
                }
            }
        }
        if (stURL->fields[HV_URL_HOST].len == 0) {
            stURL->fields[HV_URL_HOST].off = host - begin;
            stURL->fields[HV_URL_HOST].len = port - host;
        }
        if (ep == end) return 0;
        // /path
        sp = ep;
        ep = strchr(sp, '?');
        if (ep == NULL) ep = end;
        stURL->fields[HV_URL_PATH].off = sp - begin;
        stURL->fields[HV_URL_PATH].len = ep - sp;
        if (ep == end) return 0;
        // ?query
        sp = ep + 1;
        ep = strchr(sp, '#');
        if (ep == NULL) ep = end;
        stURL->fields[HV_URL_QUERY].off = sp - begin;
        stURL->fields[HV_URL_QUERY].len = ep - sp;
        if (ep == end) return 0;
        // #fragment
        sp = ep + 1;
        ep = end;
        stURL->fields[HV_URL_FRAGMENT].off = sp - begin;
        stURL->fields[HV_URL_FRAGMENT].len = ep - sp;
    }
    return 0;
}

#if _MSC_VER
int vasprintf(char** strp, const char* fmt, va_list ap)
{
    int r = -1, size = _vsnprintf(NULL, 0, fmt, ap); // This is OK

    if ((size >= 0) && (size < INT_MAX)) {
        *strp = (char*)malloc(size + 1); //+1 for null
        if (*strp) {
            r = vsprintf(*strp, fmt, ap);
            // if ((r < 0) || (r > size)) { never fail
            //     free(*strp);
            //     r = -1;
            // }
        }
    } else {
        *strp = 0;
    }

    return (r);
}

int asprintf(char** strp, const char* fmt, ...)
{
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = vasprintf(strp, fmt, ap);
    va_end(ap);
    return (r);
}
#endif


// hex string -> binary
int from_hex_string(const char* hex_string, size_t hex_string_len, unsigned char* output)
{
    static const signed char hex_table[128] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
        -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };

    size_t i,j;
    if (hex_string_len % 2 != 0)
        return -1;
    for (i = 0; (j=i + 8) < hex_string_len; i = j) {
        int high1 = hex_table[(int)hex_string[i]];
        int low1 = hex_table[(int)hex_string[i + 1]];
        int high2 = hex_table[(int)hex_string[i + 2]];
        int low2 = hex_table[(int)hex_string[i + 3]];
        int high3 = hex_table[(int)hex_string[i + 4]];
        int low3 = hex_table[(int)hex_string[i + 5]];
        int high4 = hex_table[(int)hex_string[i + 6]];
        int low4 = hex_table[(int)hex_string[i + 7]];
        if (high1 < 0 || low1 < 0 || high2 < 0 || low2 < 0 || high3 < 0 || low3 < 0 || high4 < 0 || low4 < 0)
            return -1;
        output[i / 2] = (high1 << 4) | low1;
        output[i / 2 + 1] = (high2 << 4) | low2;
        output[i / 2 + 2] = (high3 << 4) | low3;
        output[i / 2 + 3] = (high4 << 4) | low4;
    }

    // left string
    for (; i < hex_string_len; i += 2) {
        int high = hex_table[(int)hex_string[i]];
        int low = hex_table[(int)hex_string[i + 1]];
        if (high < 0 || low < 0)
            return -1;
        output[i / 2] = (high << 4) | low;
    }
    return hex_string_len/2;
}

//no alloc
void to_hex(const void* in, size_t inlen, char* out)
{
    static const char hex_table[] = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
    };
    const uint8_t* s = (const uint8_t*)in;
    while (inlen--) {
        out[2 * inlen + 1] = hex_table[s[inlen] & 0x0f];
        out[2 * inlen] = hex_table[s[inlen] >> 4];
    }
}
