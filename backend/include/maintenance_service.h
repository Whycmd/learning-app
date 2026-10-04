// 维护服务层（阶段六）
// 职责：数据备份、学情重置、临时 Word 清理
#pragma once
#include "db.h"
#include "json.h"
#include "auth.h"
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>

namespace eng {

class MaintenanceService {
  Db& db_;
  std::string backupDir_;
  std::string wordTmpDir_;
public:
  MaintenanceService(Db& db, const std::string& backupDir, const std::string& wordTmp)
    : db_(db), backupDir_(backupDir), wordTmpDir_(wordTmp) {
    ::mkdir(backupDir_.c_str(), 0755);
  }

  // 原子任务3：数据备份导出
  // 导出全量 SQL 到 backup 目录
  struct BackupResult { bool ok=false; std::string path; int sizeBytes=0; std::string error; };
  BackupResult backupDatabase() {
    BackupResult r;
    // 生成备份文件名
    time_t now = ::time(nullptr);
    struct tm* lt = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", lt);
    std::string filename = "english_learning_" + std::string(buf) + ".sql";
    std::string filepath = backupDir_ + "/" + filename;

    // 获取所有表
    auto tables = db_.query("SHOW TABLES");
    if (tables.empty()) { r.error = "no tables found"; return r; }

    std::ofstream out(filepath);
    if (!out) { r.error = "cannot create backup file"; return r; }

    out << "-- English Learning Database Backup\n";
    out << "-- Generated: " << buf << "\n\n";
    out << "SET NAMES utf8mb4;\n";
    out << "SET FOREIGN_KEY_CHECKS = 0;\n\n";

    for (auto& trow : tables) {
      std::string tableName;
      for (auto& kv : trow) { tableName = kv.second; break; }

      // 表结构
      auto createRows = db_.query("SHOW CREATE TABLE " + tableName);
      if (!createRows.empty()) {
        for (auto& kv : createRows[0]) {
          if (kv.second.find("CREATE TABLE") != std::string::npos) {
            out << "DROP TABLE IF EXISTS `" << tableName << "`;\n";
            out << kv.second << ";\n\n";
            break;
          }
        }
      }

      // 数据
      auto dataRows = db_.query("SELECT * FROM " + tableName);
      if (!dataRows.empty()) {
        for (auto& row : dataRows) {
          std::ostringstream cols, vals;
          cols << "INSERT INTO `" << tableName << "` (";
          vals << " VALUES (";
          bool first = true;
          for (auto& kv : row) {
            if (!first) { cols << ", "; vals << ", "; }
            cols << "`" << kv.first << "`";
            if (kv.second.empty()) vals << "NULL";
            else vals << "'" << Db::escape(kv.second) << "'";
            first = false;
          }
          cols << ")";
          vals << ");\n";
          out << cols.str() << vals.str();
        }
        out << "\n";
      }
    }

    out << "SET FOREIGN_KEY_CHECKS = 1;\n";
    out.close();

    struct stat st;
    if (::stat(filepath.c_str(), &st) == 0) r.sizeBytes = (int)st.st_size;
    r.ok = true;
    r.path = filepath;
    return r;
  }

  // 原子任务3：单知识点学情重置
  bool resetLearning(int knowledgeId) {
    return db_.exec(
      "UPDATE user_learning SET proficiency_level=0, review_times=0, error_times=0, "
      "next_review_time=NOW(), last_review_time=NULL "
      "WHERE user_id=" + std::to_string(RequestContext::currentUserId())
      + " AND knowledge_id=" + std::to_string(knowledgeId)
    ).affected_rows > 0;
  }

  // 原子任务3：全量学情清空
  bool clearAllLearning() {
    return db_.exec(
      "UPDATE user_learning SET proficiency_level=0, review_times=0, error_times=0, "
      "next_review_time=NOW(), last_review_time=NULL WHERE user_id="
      + std::to_string(RequestContext::currentUserId())
    ).affected_rows >= 0;
  }

  // 临时 Word 清理：删除生成超过 maxAgeSec 的文件
  struct CleanupResult { int deleted=0; int skipped=0; std::vector<std::string> deletedFiles; };
  CleanupResult cleanupTempWord(int maxAgeSec = 3600) {
    CleanupResult r;
    DIR* dir = ::opendir(wordTmpDir_.c_str());
    if (!dir) return r;
    time_t now = ::time(nullptr);
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      // 只清理习题 Word 和临时 JSON
      if (name.find("exercise_") == std::string::npos &&
          name.find("ex_input_") == std::string::npos &&
          name.find("kp_") == std::string::npos) { r.skipped++; continue; }
      std::string fullpath = wordTmpDir_ + "/" + name;
      struct stat st;
      if (::stat(fullpath.c_str(), &st) != 0) { r.skipped++; continue; }
      if (now - st.st_mtime > maxAgeSec) {
        if (::unlink(fullpath.c_str()) == 0) {
          r.deleted++;
          r.deletedFiles.push_back(name);
        } else { r.skipped++; }
      } else { r.skipped++; }
    }
    ::closedir(dir);
    return r;
  }

  // 软件启动时清空所有习题临时文件
  CleanupResult cleanupStartupExercises() {
    CleanupResult r;
    DIR* dir = ::opendir(wordTmpDir_.c_str());
    if (!dir) return r;
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      if (name.find("exercise_") == 0 || name.find("ex_input_") == 0) {
        std::string fullpath = wordTmpDir_ + "/" + name;
        if (::unlink(fullpath.c_str()) == 0) {
          r.deleted++;
          r.deletedFiles.push_back(name);
        }
      }
    }
    ::closedir(dir);
    return r;
  }

  // 获取备份文件列表
  JsonValue listBackups() {
    JsonArray arr;
    DIR* dir = ::opendir(backupDir_.c_str());
    if (!dir) return JsonValue::makeArray(arr);
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      if (name.find(".sql") == std::string::npos) continue;
      std::string fullpath = backupDir_ + "/" + name;
      struct stat st;
      if (::stat(fullpath.c_str(), &st) == 0) {
        JsonObject o;
        o["name"] = JsonValue::makeString(name);
        o["path"] = JsonValue::makeString(fullpath);
        o["size"] = JsonValue::makeInt((int)st.st_size);
        char tbuf[64];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%s", localtime(&st.st_mtime));
        o["time"] = JsonValue::makeString(tbuf);
        arr.push_back(JsonValue::makeObject(o));
      }
    }
    ::closedir(dir);
    return JsonValue::makeArray(arr);
  }
};

} // namespace eng
