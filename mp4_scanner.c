/**

@file mp4_scanner.c

@brief Утилита для рекурсивного поиска MP4-файлов в папках и подсчёта их общей длительности.



Поддерживает кроссплатформенность (Linux/Windows),

может отображать подробную информацию (-v),

извлекает длительность MP4-файлов через парсинг атомов moov/mvhd.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_GREEN "\x1b[32m"
#define COLOR_RESET "\x1b[0m"
#define CP_UTF8 65001
#else
#include <dirent.h>
#define COLOR_YELLOW "\033[33m"
#define COLOR_GREEN "\033[32m"
#define COLOR_RESET "\033[0m"
#endif

/**
 * @struct MP4Duration
 * @brief Структура для хранения длительности MP4-файла.
 */
typedef struct
{
    double duration_seconds; /**< Длительность в секундах */
    int found;               /**< Флаг, указывающий, была ли найдена длительность */
} MP4Duration;

/**

@struct Stats

@brief Статистика по найденным MP4-файлам.
*/
typedef struct
{
    int total_files;               /** < Общее количество MP4 - файлов */
    int total_folders_with_mp4;    /** < Количество папок с MP4 */
    double total_duration_seconds; /**< Общая длительность видео */
} Stats;

/**

@struct Options

@brief Опции командной строки.
*/
typedef struct
{
    int verbose; /**< Флаг подробного вывода */
} Options;

/**

@brief Чтение 4 байт в формате big-endian.
*/
uint32_t read_u32_be(FILE *file)
{
    uint8_t buf[4];
    if (fread(buf, 1, 4, file) != 4)
        return 0;
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

/**

@brief Чтение 8 байт в формате big-endian.
*/
uint64_t read_u64_be(FILE *file)
{
    uint64_t high = read_u32_be(file);
    uint64_t low = read_u32_be(file);
    return (high << 32) | low;
}

/**

@brief Поиск атома по имени.
*/
int find_atom(FILE *file, const char *atom_type, uint64_t *size, uint64_t *start_pos)
{
    char box_type[5] = {0};
    uint32_t box_size;

    while (!feof(file))
    {
        box_size = read_u32_be(file);
        if (fread(box_type, 1, 4, file) != 4)
            break;

        if (box_size == 1)
        {
            box_size = (uint32_t)read_u64_be(file);
        }

        if (strncmp(box_type, atom_type, 4) == 0)
        {
            *size = box_size;
            *start_pos = ftell(file);
            return 1;
        }
        else
        {
            if (fseek(file, box_size - 8, SEEK_CUR) != 0)
                break;
        }
    }
    return 0;
}

#ifdef _WIN32
/**
 * @brief Преобразует строку UTF-8 в UTF-16 (память выделяется через malloc).
 */
static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/**
 * @brief Преобразует строку UTF-16 в UTF-8 (память выделяется через malloc).
 */
static char *wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/**
 * @brief Строит из пути UTF-8 абсолютный "длинный" путь с префиксом \\?\
 *
 * Префикс снимает ограничение MAX_PATH (260 символов) в Win32 API.
 * Для сетевых путей (\\server\share) используется префикс \\?\UNC\
 * Результат нужно освободить через free().
 */
static wchar_t *to_long_path(const char *utf8)
{
    wchar_t *w = utf8_to_wide(utf8);
    if (!w)
        return NULL;

    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0)
    {
        free(w);
        return NULL;
    }

    wchar_t *full = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    wchar_t *out = (wchar_t *)malloc(((size_t)n + 16) * sizeof(wchar_t));
    if (!full || !out || GetFullPathNameW(w, n, full, NULL) == 0)
    {
        free(w);
        free(full);
        free(out);
        return NULL;
    }
    free(w);

    if (wcsncmp(full, L"\\\\?\\", 4) == 0)
    {
        wcscpy(out, full);
    }
    else if (wcsncmp(full, L"\\\\", 2) == 0)
    {
        wcscpy(out, L"\\\\?\\UNC\\");
        wcscat(out, full + 2);
    }
    else
    {
        wcscpy(out, L"\\\\?\\");
        wcscat(out, full);
    }
    free(full);
    return out;
}
#endif

/**

@brief Открывает файл на чтение (на Windows - с поддержкой путей длиннее 260 символов и Unicode).
*/
static FILE *open_file_rb(const char *filename)
{
#ifdef _WIN32
    wchar_t *lp = to_long_path(filename);
    if (!lp)
        return NULL;
    FILE *f = _wfopen(lp, L"rb");
    free(lp);
    return f;
#else
    return fopen(filename, "rb");
#endif
}

/**

@brief Получение длительности MP4-файла.
*/
MP4Duration get_mp4_duration(const char *filename)
{
    FILE *file = open_file_rb(filename);
    MP4Duration result = {0, 0};
    if (!file)
        return result;

    uint64_t moov_size, moov_pos;
    if (!find_atom(file, "moov", &moov_size, &moov_pos))
    {
        fclose(file);
        return result;
    }

    uint64_t mvhd_size, mvhd_pos;
    if (!find_atom(file, "mvhd", &mvhd_size, &mvhd_pos))
    {
        fclose(file);
        return result;
    }

    uint8_t version;
    fread(&version, 1, 1, file);
    fseek(file, 3, SEEK_CUR); // Пропускаем флаги

    uint32_t timescale;
    double duration;

    if (version == 1)
    {
        fseek(file, 8 + 8, SEEK_CUR);
        timescale = read_u32_be(file);
        duration = read_u64_be(file);
    }
    else
    {
        fseek(file, 4 + 4, SEEK_CUR);
        timescale = read_u32_be(file);
        duration = read_u32_be(file);
    }

    if (timescale > 0)
    {
        result.duration_seconds = duration / timescale;
        result.found = 1;
    }

    fclose(file);
    return result;
}

/**

@brief Форматирует длительность в часы, минуты и секунды.
*/
void format_duration(double total_seconds, int *hours, int *minutes, int *seconds)
{
    *hours = (int)(total_seconds / 3600);
    *minutes = (int)((total_seconds - (*hours * 3600)) / 60);
    *seconds = (int)total_seconds % 60;
}

/**

@brief Усечение длинных путей для отображения.
*/
void truncate_path(const char *input, char *output, size_t max_len)
{
    size_t len = strlen(input);
    if (len <= max_len)
    {
        strcpy(output, input);
        return;
    }

    size_t head = max_len / 2 - 2;
    size_t tail = max_len / 2 - 2;
    snprintf(output, max_len + 1, "%.*s...%.*s", (int)head, input, (int)tail, input + len - tail);
}

/**

@brief Проверяет, что имя файла заканчивается на ".mp4" (без учёта регистра).
*/
static int has_mp4_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 4)
        return 0;
    const char *ext = name + len - 4;
    return ext[0] == '.' &&
           tolower((unsigned char)ext[1]) == 'm' &&
           tolower((unsigned char)ext[2]) == 'p' &&
           ext[3] == '4';
}

/**

@brief Добавляет MP4-файл в статистику (если удалось прочитать его длительность).
*/
static void add_mp4_file(const char *full_path, Stats *stats, int *local_count, double *local_duration)
{
    MP4Duration d = get_mp4_duration(full_path);
    if (d.found)
    {
        stats->total_files++;
        (*local_count)++;
        *local_duration += d.duration_seconds;
        stats->total_duration_seconds += d.duration_seconds;
    }
    else
    {
        fprintf(stderr, "Warning: cannot read duration: %s\n", full_path);
    }
}

/**

@brief Выводит итог по папке (в режиме -v) и обновляет счётчик папок.
*/
static void report_folder(const char *path, int local_count, double local_duration, Stats *stats, Options *opts)
{
    if (local_count <= 0)
        return;

    stats->total_folders_with_mp4++;
    if (opts->verbose)
    {
        int h, m, s;
        format_duration(local_duration, &h, &m, &s);
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%d:%02d:%02d", h, m, s);

        char truncated[128];
        truncate_path(path, truncated, 90);

        printf("\xF0\x9F\x9F\xA1 %s " COLOR_GREEN "%s\n" COLOR_RESET, time_str, truncated);
    }
}

/**

@brief Склеивает путь папки и имя элемента (память выделяется через malloc).
*/
static char *join_path(const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    char *r = (char *)malloc(dl + nl + 2);
    if (!r)
        return NULL;
    memcpy(r, dir, dl);
    if (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\')
        r[dl++] = '/';
    memcpy(r + dl, name, nl + 1);
    return r;
}

#ifdef _WIN32

/**

@brief Рекурсивное сканирование директории (Windows: Unicode и пути длиннее 260 символов).
*/
void scan_directory(const char *path, Stats *stats, Options *opts)
{
    int local_mp4_count = 0;
    double local_duration = 0.0;

    wchar_t *lp = to_long_path(path);
    if (!lp)
    {
        fprintf(stderr, "Error: bad path: %s\n", path);
        return;
    }

    size_t len = wcslen(lp);
    wchar_t *pattern = (wchar_t *)malloc((len + 3) * sizeof(wchar_t));
    if (!pattern)
    {
        free(lp);
        return;
    }
    wcscpy(pattern, lp);
    if (len > 0 && pattern[len - 1] != L'\\')
        pattern[len++] = L'\\';
    pattern[len] = L'*';
    pattern[len + 1] = L'\0';
    free(lp);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Error: cannot open folder (code %lu): %s\n", (unsigned long)GetLastError(), path);
        return;
    }

    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;

        char *name = wide_to_utf8(fd.cFileName);
        if (!name)
            continue;
        char *full_path = join_path(path, name);
        free(name);
        if (!full_path)
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            /* Символьные ссылки и junction пропускаем, чтобы не уйти в цикл */
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                scan_directory(full_path, stats, opts);
        }
        else
        {
            char *base = wide_to_utf8(fd.cFileName);
            if (base && has_mp4_ext(base))
                add_mp4_file(full_path, stats, &local_mp4_count, &local_duration);
            free(base);
        }
        free(full_path);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    report_folder(path, local_mp4_count, local_duration, stats, opts);
}

#else

/**

@brief Рекурсивное сканирование директории.
*/
void scan_directory(const char *path, Stats *stats, Options *opts)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    struct stat st;
    int local_mp4_count = 0;
    double local_duration = 0.0;

    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        char *full_path = join_path(path, entry->d_name);
        if (!full_path)
            continue;

        if (stat(full_path, &st) != -1)
        {
            if (S_ISDIR(st.st_mode))
                scan_directory(full_path, stats, opts);
            else if (S_ISREG(st.st_mode) && has_mp4_ext(entry->d_name))
                add_mp4_file(full_path, stats, &local_mp4_count, &local_duration);
        }
        free(full_path);
    }

    closedir(dir);
    report_folder(path, local_mp4_count, local_duration, stats, opts);
}

#endif

/**

@brief Точка входа в программу.
*/
int main(int argc, char *argv[])
{
    Stats stats = {0, 0, 0.0};
    Options opts = {0};
#ifndef _WIN32
    char path[PATH_MAX] = {0};
#endif
    const char *target_dir = NULL;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);

    // Аргументы берём в UTF-16 и переводим в UTF-8 (argv в ANSI ломает Unicode-пути)
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **u8argv = NULL;
    if (wargv)
    {
        u8argv = (char **)calloc((size_t)wargc + 1, sizeof(char *));
        if (u8argv)
        {
            for (int i = 0; i < wargc; ++i)
                u8argv[i] = wide_to_utf8(wargv[i]);
            argc = wargc;
            argv = u8argv;
        }
    }
    char *cwd8 = NULL;
#endif

    // Обработка аргументов командной строки
    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i])
            continue;
        if (strcmp(argv[i], "-v") == 0)
        {
            opts.verbose = 1;
        }
        else
        {
            target_dir = argv[i];
        }
    }

    if (!target_dir)
    {
#ifdef _WIN32
        DWORD need = GetCurrentDirectoryW(0, NULL);
        wchar_t *wcwd = need ? (wchar_t *)malloc((size_t)need * sizeof(wchar_t)) : NULL;
        if (!wcwd || GetCurrentDirectoryW(need, wcwd) == 0 || !(cwd8 = wide_to_utf8(wcwd)))
        {
            fprintf(stderr, "GetCurrentDirectory failed\n");
            return 1;
        }
        free(wcwd);
        target_dir = cwd8;
#else
        if (!getcwd(path, sizeof(path)))
        {
            perror("getcwd failed");
            return 1;
        }
        target_dir = path;
#endif
    }

    printf("\xF0\x9F\x95\x92 Scanning folder: %s\n", target_dir);
    scan_directory(target_dir, &stats, &opts);

    int h, m, s;
    format_duration(stats.total_duration_seconds, &h, &m, &s);

    printf("\n\xF0\x9F\x93\x8A Result:\n");
    printf("\xF0\x9F\x91\x8C Found " COLOR_YELLOW "%d" COLOR_RESET " MP4 files in " COLOR_YELLOW "%d" COLOR_RESET " folders.\n",
           stats.total_files, stats.total_folders_with_mp4);
    printf("\xF0\x9F\x8F\x81 Total duration: " COLOR_YELLOW "%d:%02d:%02d" COLOR_RESET "\n", h, m, s);

    return 0;
}
