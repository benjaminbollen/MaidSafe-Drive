/*  Copyright 2013 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#ifndef MAIDSAFE_DRIVE_DIRECTORY_H_
#define MAIDSAFE_DRIVE_DIRECTORY_H_

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "boost/asio/io_service.hpp"
#include "boost/asio/steady_timer.hpp"
#include "boost/filesystem/path.hpp"
#include "boost/system/error_code.hpp"

#include "maidsafe/common/tagged_value.h"
#include "maidsafe/common/types.h"
#include "maidsafe/common/data_types/immutable_data.h"
#include "maidsafe/common/data_types/structured_data_versions.h"

#include "maidsafe/drive/config.h"
#include "maidsafe/drive/file_context.h"

namespace maidsafe {

namespace drive {

namespace detail {

class Directory;

namespace test {

void DirectoriesMatch(const Directory& lhs, const Directory& rhs);
class DirectoryTest;

}  // namespace test

class Directory {
 public:
  Directory(ParentId parent_id, DirectoryId directory_id, boost::asio::io_service& io_service,
            std::function<void(Directory*)> put_functor,  // NOLINT
            const boost::filesystem::path& path);  // NOLINT
  Directory(ParentId parent_id, const std::string& serialised_directory,
            const std::vector<StructuredDataVersions::VersionName>& versions,
            boost::asio::io_service& io_service, std::function<void(Directory*)> put_functor,  // NOLINT
            const boost::filesystem::path& path);
  ~Directory();
  // This marks the start of an attempt to store the directory.  It serialises the appropriate
  // member data (critically parent_id_ must never be serialised), and sets 'store_state_' to
  // kOngoing.  It also calls 'FlushChild' on all children (see below).
  std::string Serialise();
  // Stores all new chunks from 'child', increments all the other chunks, and resets child's
  // self_encryptor & buffer.
  void FlushChildAndDeleteEncryptor(FileContext* child);

  size_t VersionsCount() const;
  std::tuple<DirectoryId, StructuredDataVersions::VersionName>
      InitialiseVersions(ImmutableData::Name version_id);
  // This marks the end of an attempt to store the directory.  It returns directory_id and most
  // recent 2 version names (including the one passed in), and sets 'store_state_' to kComplete.
  std::tuple<DirectoryId, StructuredDataVersions::VersionName, StructuredDataVersions::VersionName>
      AddNewVersion(ImmutableData::Name version_id);

  bool HasChild(const boost::filesystem::path& name) const;
  const FileContext* GetChild(const boost::filesystem::path& name) const;
  FileContext* GetMutableChild(const boost::filesystem::path& name);
  const FileContext* GetChildAndIncrementCounter();
  void AddChild(FileContext&& child);
  FileContext RemoveChild(const boost::filesystem::path& name);
  void RenameChild(const boost::filesystem::path& old_name,
                   const boost::filesystem::path& new_name);
  void ResetChildrenCounter();
  bool empty() const;
  ParentId parent_id() const;
  // This will block while a store attempt is ongoing.
  void SetNewParent(const ParentId parent_id, std::function<void(Directory*)> put_functor,  // NOLINT
                    const boost::filesystem::path& path);
  DirectoryId directory_id() const;
  void ScheduleForStoring();
  void StoreImmediatelyIfPending();

  friend void test::DirectoriesMatch(const Directory& lhs, const Directory& rhs);
  friend class test::DirectoryTest;

  // TODO(Fraser#5#): 2014-01-30 - BEFORE_RELEASE - Make mutex_ private.
  mutable std::mutex mutex_;

 private:
  Directory(const Directory& other);
  Directory(Directory&& other);
  Directory& operator=(Directory other);

  typedef std::vector<std::unique_ptr<FileContext>> Children;

  Children::iterator Find(const boost::filesystem::path& name);
  Children::const_iterator Find(const boost::filesystem::path& name) const;
  void SortAndResetChildrenCounter();
  void DoScheduleForStoring(bool use_delay = true);

  std::condition_variable cond_var_;
  ParentId parent_id_;
  DirectoryId directory_id_;
  boost::asio::steady_timer timer_;
  std::function<void(const boost::system::error_code&)> store_functor_;
  std::deque<StructuredDataVersions::VersionName> versions_;
  MaxVersions max_versions_;
  Children children_;
  size_t children_count_position_;
  enum class StoreState { kPending, kOngoing, kComplete } store_state_;
};

bool operator<(const Directory& lhs, const Directory& rhs);

}  // namespace detail

}  // namespace drive

}  // namespace maidsafe

#endif  // MAIDSAFE_DRIVE_DIRECTORY_H_
