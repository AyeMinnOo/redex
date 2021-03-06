// Copyright 2004-present Facebook. All Rights Reserved.

#include "ApkManager.h"

#include <boost/filesystem.hpp>
#include <iostream>
#include <sstream>

namespace {

void check_directory(std::string& dir) {
  if (!boost::filesystem::is_directory(dir.c_str())) {
    std::cerr << "error: not a writable directory: " << dir
              << std::endl;
    exit(EXIT_FAILURE);
  }
}

}

FILE* ApkManager::new_asset_file(const char* filename) {
  check_directory(m_apk_dir);
  std::ostringstream path;
  path << m_apk_dir << "/assets/";
  std::string assets_dir = path.str();
  check_directory(assets_dir);
  path << filename;

  FILE* fd = fopen(path.str().c_str(), "w");
  if (fd != nullptr) {
    m_files.emplace_back(fd);
  } else {
    perror("Error creating new asset file");
  }
  return fd;
}
