// Copyright (c) 2016 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "brave/utility/importer/chrome_importer.h"

#include <memory>
#include <string>

#include "brave/utility/importer/brave_external_process_importer_bridge.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task_scheduler/post_task.h"
#include "base/task_scheduler/task_traits.h"
#include "base/values.h"
#include "brave/common/importer/imported_cookie_entry.h"
#include "build/build_config.h"
#include "chrome/common/importer/imported_bookmark_entry.h"
#include "chrome/common/importer/importer_bridge.h"
#include "chrome/common/importer/importer_url_row.h"
#include "chrome/utility/importer/favicon_reencode.h"
#include "components/autofill/core/common/password_form.h"
#include "components/os_crypt/os_crypt.h"
#include "components/password_manager/core/browser/login_database.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/json_pref_store.h"
#include "components/prefs/pref_filter.h"
#include "sql/connection.h"
#include "sql/statement.h"
#include "url/gurl.h"

#if defined(USE_X11)
#if defined(USE_LIBSECRET)
#include "chrome/browser/password_manager/native_backend_libsecret.h"
#endif
#include "chrome/browser/password_manager/native_backend_kwallet_x.h"
#include "chrome/browser/password_manager/password_store_x.h"
#include "components/os_crypt/key_storage_util_linux.h"

base::nix::DesktopEnvironment ChromeImporter::GetDesktopEnvironment() {
  std::unique_ptr<base::Environment> env(base::Environment::Create());
  return base::nix::GetDesktopEnvironment(env.get());
}
#endif

ChromeImporter::ChromeImporter() {
}

ChromeImporter::~ChromeImporter() {
}

void ChromeImporter::StartImport(const importer::SourceProfile& source_profile,
                                  uint16_t items,
                                  ImporterBridge* bridge) {
  bridge_ = bridge;
  source_path_ = source_profile.source_path;

  // The order here is important!
  bridge_->NotifyStarted();

  if ((items & importer::HISTORY) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::HISTORY);
    ImportHistory();
    bridge_->NotifyItemEnded(importer::HISTORY);
  }

  if ((items & importer::FAVORITES) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::FAVORITES);
    ImportBookmarks();
    bridge_->NotifyItemEnded(importer::FAVORITES);
  }

  if ((items & importer::COOKIES) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::COOKIES);
    ImportCookies();
    bridge_->NotifyItemEnded(importer::COOKIES);
  }

  if ((items & importer::PASSWORDS) && !cancelled()) {
    bridge_->NotifyItemStarted(importer::PASSWORDS);
    ImportPasswords();
    bridge_->NotifyItemEnded(importer::PASSWORDS);
  }

  bridge_->NotifyEnded();
}

void ChromeImporter::ImportHistory() {
  base::FilePath history_path =
    source_path_.Append(
      base::FilePath::StringType(FILE_PATH_LITERAL("History")));
  if (!base::PathExists(history_path))
    return;

  sql::Connection db;
  if (!db.Open(history_path))
    return;

  const char query[] =
    "SELECT url, title, last_visit_time, typed_count, visit_count "
    "FROM urls WHERE hidden = 0";

  sql::Statement s(db.GetUniqueStatement(query));

  std::vector<ImporterURLRow> rows;
  while (s.Step() && !cancelled()) {
    GURL url(s.ColumnString(0));

    ImporterURLRow row(url);
    row.title = s.ColumnString16(1);
    row.last_visit =
      base::Time::FromDoubleT(chromeTimeToDouble((s.ColumnInt64(2))));
    row.hidden = false;
    row.typed_count = s.ColumnInt(3);
    row.visit_count = s.ColumnInt(4);

    rows.push_back(row);
  }

  if (!rows.empty() && !cancelled())
    bridge_->SetHistoryItems(rows, importer::VISIT_SOURCE_CHROME_IMPORTED);
}

void ChromeImporter::ImportBookmarks() {
  std::string bookmarks_content;
  base::FilePath bookmarks_path =
    source_path_.Append(
      base::FilePath::StringType(FILE_PATH_LITERAL("Bookmarks")));
  base::ReadFileToString(bookmarks_path, &bookmarks_content);
  std::unique_ptr<base::Value> bookmarks_json =
    base::JSONReader::Read(bookmarks_content);
  const base::DictionaryValue* bookmark_dict;
  if (!bookmarks_json || !bookmarks_json->GetAsDictionary(&bookmark_dict))
    return;
  std::vector<ImportedBookmarkEntry> bookmarks;
  const base::DictionaryValue* roots;
  const base::DictionaryValue* bookmark_bar;
  const base::DictionaryValue* other;
  if (bookmark_dict->GetDictionary("roots", &roots)) {
    // Importing bookmark bar items
    if (roots->GetDictionary("bookmark_bar", &bookmark_bar)) {
      std::vector<base::string16> path;
      base::string16 name;
      bookmark_bar->GetString("name", &name);

      path.push_back(name);
      RecursiveReadBookmarksFolder(bookmark_bar, path, true, &bookmarks);
    }
    // Importing other items
    if (roots->GetDictionary("other", &other)) {
      std::vector<base::string16> path;
      base::string16 name;
      other->GetString("name", &name);

      path.push_back(name);
      RecursiveReadBookmarksFolder(other, path, false, &bookmarks);
    }
  }
  // Write into profile.
  if (!bookmarks.empty() && !cancelled()) {
    const base::string16& first_folder_name =
      base::UTF8ToUTF16("Imported from Chrome");
    bridge_->AddBookmarks(bookmarks, first_folder_name);
  }

  // Import favicons.
  base::FilePath favicons_path =
    source_path_.Append(
      base::FilePath::StringType(FILE_PATH_LITERAL("Favicons")));
  if (!base::PathExists(favicons_path))
    return;

  sql::Connection db;
  if (!db.Open(favicons_path))
    return;

  FaviconMap favicon_map;
  ImportFaviconURLs(&db, &favicon_map);
  // Write favicons into profile.
  if (!favicon_map.empty() && !cancelled()) {
    favicon_base::FaviconUsageDataList favicons;
    LoadFaviconData(&db, favicon_map, &favicons);
    bridge_->SetFavicons(favicons);
  }
}

void ChromeImporter::ImportFaviconURLs(
  sql::Connection* db,
  FaviconMap* favicon_map) {
  const char query[] = "SELECT icon_id, page_url FROM icon_mapping;";
  sql::Statement s(db->GetUniqueStatement(query));

  while (s.Step() && !cancelled()) {
    int64_t icon_id = s.ColumnInt64(0);
    GURL url = GURL(s.ColumnString(1));
    (*favicon_map)[icon_id].insert(url);
  }
}

void ChromeImporter::LoadFaviconData(
    sql::Connection* db,
    const FaviconMap& favicon_map,
    favicon_base::FaviconUsageDataList* favicons) {
  const char query[] = "SELECT url "
                       "FROM favicons "
                       "WHERE id = ?;";
  sql::Statement s(db->GetUniqueStatement(query));

  for (FaviconMap::const_iterator i = favicon_map.begin();
       i != favicon_map.end(); ++i) {
    s.Reset(true);
    s.BindInt64(0, i->first);
    if (s.Step()) {
      favicon_base::FaviconUsageData usage;

      GURL url = GURL(s.ColumnString(0));
      if (url.is_valid()) {
        if (url.SchemeIs(url::kDataScheme)) {
          std::vector<unsigned char> data;
          s.ColumnBlobAsVector(0, &data);
          if (data.empty()) {
            continue;  // Data definitely invalid.
          }
          if (!importer::ReencodeFavicon(&data[0], data.size(),
                                         &usage.png_data))
            continue;  // Unable to decode.
        } else {
          usage.favicon_url = url;
        }
      } else {
        continue;  // Don't bother importing favicons with invalid URLs.
      }

      usage.urls = i->second;
      favicons->push_back(usage);
    }
  }
}

void ChromeImporter::ImportCookies() {
  base::FilePath cookies_path =
    source_path_.Append(
      base::FilePath::StringType(FILE_PATH_LITERAL("Cookies")));
  if (!base::PathExists(cookies_path))
    return;

  sql::Connection db;
  if (!db.Open(cookies_path))
    return;

  const char query[] =
    "SELECT host_key, name, value, path, expires_utc, secure, httponly, "
    "encrypted_value FROM cookies WHERE length(encrypted_value) = 0";

  sql::Statement s(db.GetUniqueStatement(query));

  std::vector<ImportedCookieEntry> cookies;
  while (s.Step() && !cancelled()) {
    ImportedCookieEntry cookie;
    base::string16 host(base::UTF8ToUTF16("*"));
    host.append(s.ColumnString16(0));
    cookie.domain = s.ColumnString16(0);
    cookie.name = s.ColumnString16(1);
    cookie.value = s.ColumnString16(2);
    cookie.host = host;
    cookie.path = s.ColumnString16(3);
    cookie.expiry_date =
      base::Time::FromDoubleT(chromeTimeToDouble((s.ColumnInt64(4))));
    cookie.secure = s.ColumnBool(5);
    cookie.httponly = s.ColumnBool(6);

    cookies.push_back(cookie);
  }

  if (!cookies.empty() && !cancelled())
    static_cast<BraveExternalProcessImporterBridge*>(bridge_.get())->
        SetCookies(cookies);
}

void ChromeImporter::ImportPasswords() {
#if !defined(USE_X11)
  base::FilePath passwords_path =
    source_path_.Append(
      base::FilePath::StringType(FILE_PATH_LITERAL("Login Data")));

  password_manager::LoginDatabase database(passwords_path);
  if (!database.Init()) {
    LOG(ERROR) << "LoginDatabase Init() failed";
    return;
  }

  std::vector<std::unique_ptr<autofill::PasswordForm>> forms;
  bool success = database.GetAutofillableLogins(&forms);
  if (success) {
    for (int i = 0; i < forms.size(); ++i) {
      bridge_->SetPasswordForm(*forms[i].get());
    }
  }
  std::vector<std::unique_ptr<autofill::PasswordForm>> blacklist;
  success = database.GetBlacklistLogins(&blacklist);
  if (success) {
    for (int i = 0; i < blacklist.size(); ++i) {
      bridge_->SetPasswordForm(*blacklist[i].get());
    }
  }
#else
  base::FilePath prefs_path =
    source_path_.Append(
      base::FilePath::StringType(FILE_PATH_LITERAL("Preferences")));
  const base::Value *value;
  scoped_refptr<base::SequencedTaskRunner> file_task_runner =
    base::CreateSequencedTaskRunnerWithTraits(base::TaskTraits().MayBlock());
  scoped_refptr<JsonPrefStore> prefs = new JsonPrefStore(
      prefs_path, file_task_runner, std::unique_ptr<PrefFilter>());
  int local_profile_id;
  if (prefs->ReadPrefs() != PersistentPrefStore::PREF_READ_ERROR_NONE) {
    return;
  }
  if (!prefs->GetValue(password_manager::prefs::kLocalProfileId, &value)) {
    return;
  }
  if (!value->GetAsInteger(&local_profile_id)) {
    return;
  }

  std::unique_ptr<PasswordStoreX::NativeBackend> backend;
  base::nix::DesktopEnvironment desktop_env = GetDesktopEnvironment();

  os_crypt::SelectedLinuxBackend selected_backend =
      os_crypt::SelectBackend(std::string(), desktop_env);
  if (!backend &&
      (selected_backend == os_crypt::SelectedLinuxBackend::KWALLET ||
      selected_backend == os_crypt::SelectedLinuxBackend::KWALLET5)) {
    base::nix::DesktopEnvironment used_desktop_env =
        selected_backend == os_crypt::SelectedLinuxBackend::KWALLET
            ? base::nix::DESKTOP_ENVIRONMENT_KDE4
            : base::nix::DESKTOP_ENVIRONMENT_KDE5;
    backend.reset(new NativeBackendKWallet(local_profile_id,
                                           used_desktop_env));
  } else if (selected_backend == os_crypt::SelectedLinuxBackend::GNOME_ANY ||
             selected_backend ==
                 os_crypt::SelectedLinuxBackend::GNOME_KEYRING ||
             selected_backend ==
                 os_crypt::SelectedLinuxBackend::GNOME_LIBSECRET) {
#if defined(USE_LIBSECRET)
    if (!backend &&
        (selected_backend == os_crypt::SelectedLinuxBackend::GNOME_ANY ||
        selected_backend == os_crypt::SelectedLinuxBackend::GNOME_LIBSECRET)) {
      backend.reset(new NativeBackendLibsecret(local_profile_id));
    }
#endif
  }
  if (backend && backend->Init()) {
    std::vector<std::unique_ptr<autofill::PasswordForm>> forms;
    bool success = backend->GetAutofillableLogins(&forms);
    if (success) {
      for (int i = 0; i < forms.size(); ++i) {
        bridge_->SetPasswordForm(*forms[i].get());
      }
    }
    std::vector<std::unique_ptr<autofill::PasswordForm>> blacklist;
    success = backend->GetBlacklistLogins(&blacklist);
    if (success) {
      for (int i = 0; i < blacklist.size(); ++i) {
        bridge_->SetPasswordForm(*blacklist[i].get());
      }
    }
  }
#endif
}

void ChromeImporter::RecursiveReadBookmarksFolder(
  const base::DictionaryValue* folder,
  const std::vector<base::string16>& parent_path,
  bool is_in_toolbar,
  std::vector<ImportedBookmarkEntry>* bookmarks) {
  const base::ListValue* children;
  if (folder->GetList("children", &children)) {
    for (const auto& value : *children) {
      const base::DictionaryValue* dict;
      if (!value.GetAsDictionary(&dict))
        continue;
      std::string date_added, type, url;
      base::string16 name;
      dict->GetString("date_added", &date_added);
      dict->GetString("name", &name);
      dict->GetString("type", &type);
      dict->GetString("url", &url);
      ImportedBookmarkEntry entry;
      if (type == "folder") {
        entry.in_toolbar = is_in_toolbar;
        entry.is_folder = true;
        entry.url = GURL();
        entry.path = parent_path;
        entry.title = name;
        entry.creation_time =
          base::Time::FromDoubleT(chromeTimeToDouble(std::stoll(date_added)));
        bookmarks->push_back(entry);

        std::vector<base::string16> path = parent_path;
        path.push_back(name);
        RecursiveReadBookmarksFolder(dict, path, false, bookmarks);
      } else if (type == "url") {
        entry.in_toolbar = is_in_toolbar;
        entry.is_folder = false;
        entry.url = GURL(url);
        entry.path = parent_path;
        entry.title = name;
        entry.creation_time =
          base::Time::FromDoubleT(chromeTimeToDouble(std::stoll(date_added)));
        bookmarks->push_back(entry);
      }
    }
  }
}

double ChromeImporter::chromeTimeToDouble(int64_t time) {
  return ((time * 10 - 0x19DB1DED53E8000) / 10000) / 1000;
}
