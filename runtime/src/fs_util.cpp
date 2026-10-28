// Small portable filesystem helpers.
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
void list_dir(const char* dir, std::vector<std::string>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(dir) + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
#include <dirent.h>
void list_dir(const char* dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir);
    if (!d) return;
    while (dirent* e = readdir(d)) if (e->d_name[0] != '.') out.push_back(e->d_name);
    closedir(d);
}
#endif
