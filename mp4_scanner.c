#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_GREEN "\x1b[32m"
#define COLOR_RESET "\x1b[0m"
#else
#define COLOR_YELLOW "\033[33m"
#define COLOR_GREEN "\033[32m"
#define COLOR_RESET "\033[0m"
#endif

typedef struct
{
    double duration_seconds;
    int found;
} MP4Duration;

typedef struct
{
    int total_files;
    int total_folders_with_mp4;
    double total_duration_seconds;
} Stats;

typedef struct
{
    int verbose;
} Options;

uint32_t read_u32_be(FILE *file)
{
    uint8_t buf[4];
    if (fread(buf, 1, 4, file) != 4)
        return 0;
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

uint64_t read_u64_be(FILE *file)
{
    uint64_t high = read_u32_be(file);
    uint64_t low = read_u32_be(file);
    return (high << 32) | low;
}

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
                break; // Исправлено: удалено "blijft"
        }
    }
    return 0;
}

MP4Duration get_mp4_duration(const char *filename)
{
    FILE *file = fopen(filename, "rb");
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
    fseek(file, 3, SEEK_CUR); // flags

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

void format_duration(double total_seconds, int *hours, int *minutes, int *seconds)
{
    *hours = (int)(total_seconds / 3600);
    *minutes = (int)((total_seconds - (*hours * 3600)) / 60);
    *seconds = (int)total_seconds % 60;
}

void truncate_path(const char *input, char *output, size_t max_len)
{
    size_t len = strlen(input);
    if (len <= max_len)
    {
        strcpy(output, input);
        return;
    }

#ifdef _WIN32
    size_t head = max_len / 2;
    size_t tail = max_len / 2;
    snprintf(output, max_len + 1, "%.*s...%.*s", (int)head, input, (int)tail, input + len - tail);
#else
    size_t head = max_len / 2 - 2;
    size_t tail = max_len / 2 - 2;
    snprintf(output, max_len + 1, "%.*s...%.*s", (int)head, input, (int)tail, input + len - tail);
#endif
}

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

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (stat(full_path, &st) == -1)
            continue;

        if (S_ISDIR(st.st_mode))
        {
            scan_directory(full_path, stats, opts);
        }
        else if (S_ISREG(st.st_mode))
        {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, ".mp4") == 0)
            {
                MP4Duration d = get_mp4_duration(full_path);
                if (d.found)
                {
                    stats->total_files++;
                    local_mp4_count++;
                    local_duration += d.duration_seconds;
                    stats->total_duration_seconds += d.duration_seconds;
                }
            }
        }
    }

    if (local_mp4_count > 0)
    {
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

    closedir(dir);
}

int main(int argc, char *argv[])
{
    Stats stats = {0, 0, 0.0};
    Options opts = {0};
    char path[PATH_MAX] = {0};
    const char *target_dir = NULL;

    // Parse args
    for (int i = 1; i < argc; ++i)
    {
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
        if (!getcwd(path, sizeof(path)))
        {
            perror("getcwd failed");
            return 1;
        }
        target_dir = path;
    }

    printf("\xF0\x9F\x95\x92 Scanning folder: %s\n", target_dir);
    scan_directory(target_dir, &stats, &opts);

    int h, m, s;
    format_duration(stats.total_duration_seconds, &h, &m, &s);

    printf("\n\xF0\x9F\x93\x8A Result:\n");
    printf("\xF0\x9F\x91\x8C Found " COLOR_YELLOW "%d" COLOR_RESET " MP4 files in " COLOR_YELLOW "%d" COLOR_RESET " folders.\n",
           stats.total_files, stats.total_folders_with_mp4); // Исправлено: stats-> на stats.
    printf("\xF0\x9F\x8F\x81 Total duration: " COLOR_YELLOW "%d:%02d:%02d" COLOR_RESET "\n", h, m, s);

    return 0;
}