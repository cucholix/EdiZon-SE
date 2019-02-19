#include "update_manager.hpp"

#include "guis/gui.hpp"

#include <iostream>
#include <fstream>
#include <cstring>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include <filesystem>

#include "json.hpp"
#include "sha256.h"

#define API_VERSION "v2"

#define EDIZON_URL "http://api.edizon.werwolv.net/" API_VERSION

using json = nlohmann::json;

extern char* g_edizonPath;

static CURL *curl;

UpdateManager::UpdateManager() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();

  if (!curl)
    printf("Curl initialization failed!\n");
}

UpdateManager::~UpdateManager() {
  curl_easy_cleanup(curl);
  curl_global_cleanup();
}

size_t writeToString(void *contents, size_t size, size_t nmemb, void *userp){
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

size_t writeToFile(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

void deleteFile(std::string path) {
  printf("Deleting %s.\n", path.c_str());

  remove(path.c_str());
  
}

void mkPath(std::string s, mode_t mode) {
  char *str = new char[s.size() + 2];
  strcpy(str, s.c_str());
  strcat(str, "/");

  char *delim = strchr(str + 1, '/');

  do {
    *delim = '\0';

    mkdir(str, mode);

    *delim = '/';
    delim = strchr(delim + 1, '/');
  } while (delim != nullptr);
}

char* barray2hexstr (const unsigned char* data, size_t datalen) {
  size_t final_len = datalen * 2;
  char* chrs = (char *) malloc((final_len + 1) * sizeof(*chrs));
  unsigned int j = 0;

  for(j = 0; j < datalen; j++) {
    chrs[2 * j] = (data[j] >> 4) + 48;
    chrs[2 * j + 1] = (data[j] & 15 ) + 48;
    if (chrs[2 * j] > 57) chrs[2 * j] += 7;
    if (chrs[2 * j + 1] > 57) chrs[2 * j + 1] += 7;
  }
  chrs[2 * j] = '\0';

  return chrs;
}

char *dirname (char *path) {
  static const char dot[] = ".";
  char *last_slash;

  last_slash = path != NULL ? strrchr (path, '/') : NULL;

  if (last_slash == path)
    ++last_slash;
  else if (last_slash != NULL && last_slash[1] == '\0')
    last_slash = (char*)memchr (path, last_slash - path, '/');

  if (last_slash != NULL)
    last_slash[0] = '\0';
  else
    path = (char *) dot;

  return path;
}

void updateFile(std::string path) {
  printf("Updating %s.\n", path.c_str());

  std::string url = EDIZON_URL;
  url.append(path);

  FILE* fp;

  char *cPath = new char[path.length() + 1];
  strcpy(cPath, path.c_str());
  
  mkPath(dirname(cPath), 0777);

  fp = fopen(path.c_str(), "wb");

  delete[] cPath;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

  CURLcode res = curl_easy_perform(curl);

  fclose(fp);

  if (res != CURLE_OK) {
    printf("Update download CURL perform failed: %s\n", curl_easy_strerror(res));
    deleteFile(path.c_str());
  }
}

void deleteRemovedFiles(char *path) {
  DIR* dir;
  struct dirent *ent;
    if((dir=opendir(path)) != NULL){
      while (( ent = readdir(dir)) != NULL){
        if(ent->d_type == DT_DIR && strcmp(ent->d_name, ".") != 0  && strcmp(ent->d_name, "..") != 0){
          printf("%s\n", ent->d_name);
          deleteRemovedFiles(ent->d_name);
        }
      }
      closedir(dir);
    }
}

Updates UpdateManager::checkUpdate() {
  if (!curl)
    return ERROR;

  CURLcode res;
  std::string str;

  curl_easy_setopt(curl, CURLOPT_URL, EDIZON_URL "/versionlist.php");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &str);

  res = curl_easy_perform(curl);

  if (res != CURLE_OK) {
    printf("Version check CURL perform failed: %s\n", curl_easy_strerror(res));
    return ERROR;
  }

  if (str.compare(0, 1, "{") != 0) {
    printf("Invalid downloaded update file!\n");
    return ERROR;
  }

  json remote = json::parse(str);
  Updates updatedFile = NONE;

  u32 fileCnt = std::distance(remote.begin(), remote.end());
  u32 progress = 0;

	for (json::iterator iter = remote.begin(); iter != remote.end(); ++iter) {
    FILE *fp = fopen(iter.key().c_str(), "rb");

    progress++;

    if (fp != nullptr) {
      char *content;
      size_t fileSize;
      u8 fileHash[0x20];

      if (Gui::g_currMessageBox != nullptr)
        Gui::g_currMessageBox->setProgress((static_cast<float>(progress) / fileCnt) * 100);

      fseek(fp, 0, SEEK_END);
      fileSize = ftell(fp);
      rewind(fp);

      content = new char[fileSize];

      fread(content, 1, fileSize, fp);

      struct sha256_state sha_ctx;
      sha256_init(&sha_ctx);
      sha256_update(&sha_ctx, (u8 *)content, fileSize);
      sha256_finalize(&sha_ctx);
      sha256_finish(&sha_ctx, fileHash);

      delete[] content;
      fclose(fp);

      char *fileHashStr = barray2hexstr(fileHash, 0x20);

      if (strcmp(fileHashStr, iter.value().get<std::string>().c_str()) == 0) {
        free(fileHashStr);
        continue;
      }

      free(fileHashStr);
    }

    updateFile(iter.key());
    
    if (updatedFile == NONE)
      updatedFile = EDITOR;

    if (iter.key().find("EdiZon.nro") != std::string::npos)
      updatedFile = EDIZON;
  }

  std::filesystem::recursive_directory_iterator end;
  for (std::filesystem::recursive_directory_iterator it("/EdiZon/editor"); it != end; ++it) {
    if (remote[it->path().c_str()] == nullptr) {
      if (!it->is_directory()) {
        deleteFile(it->path().c_str());

        if (updatedFile == NONE)
          updatedFile = EDITOR;
      }
    }
  }

  return updatedFile;
}