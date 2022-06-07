#ifndef CO_LOG_H_INCLUDED
#define CO_LOG_H_INCLUDED

#include <coldforce/core/co.h>
#include <coldforce/core/co_mutex.h>

CO_EXTERN_C_BEGIN

//---------------------------------------------------------------------------//
// log
//---------------------------------------------------------------------------//

enum co_log_level_en
{
    CO_LOG_LEVEL_NONE = 0,
    CO_LOG_LEVEL_ERROR,
    CO_LOG_LEVEL_WARNING,
    CO_LOG_LEVEL_INFO,
    CO_LOG_LEVEL_DEBUG,

    CO_LOG_LEVEL_MAX
};

#define CO_LOG_CATEGORY_MAX             255

#define CO_LOG_CATEGORY_USER_MIN        0
#define CO_LOG_CATEGORY_USER_MAX        31

#define CO_LOG_CATEGORY_USER_DEFAULT    CO_LOG_CATEGORY_USER_MIN
#define CO_LOG_CATEGORY_NAME_USER_DEFAULT   "USER"

typedef struct co_log_t
{
    int level;

    struct Category
    {
        bool enable;
        const char* name;

    } category[CO_LOG_CATEGORY_MAX + 1];

    void* output;
    co_mutex_t* mutex;

} co_log_t;

CO_API co_log_t* co_log_get_default(void); 

CO_API void co_log_write_header(int level, int category);

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

CO_API void co_log_set_level(int level);
CO_API int co_log_get_level(void);

CO_API void co_log_set_enable(int category, bool enable);
CO_API bool co_log_get_enable(int category);

CO_API void co_log_add_category(int category, const char* name);

CO_API void co_log_write(int level, int category, const char* format, ...);

#define co_log_error(category, format, ...) \
    co_log_write(CO_LOG_LEVEL_ERROR, category, format, ##__VA_ARGS__)

#define co_log_warning(category, format, ...) \
    co_log_write(CO_LOG_LEVEL_WARNING, category, format, ##__VA_ARGS__)

#define co_log_info(category, format, ...) \
    co_log_write(CO_LOG_LEVEL_INFO, category, format, ##__VA_ARGS__)

#define co_log_debug(category, format, ...) \
    co_log_write(CO_LOG_LEVEL_DEBUG, category, format, ##__VA_ARGS__)

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

CO_EXTERN_C_END

#endif // CO_LOG_H_INCLUDED