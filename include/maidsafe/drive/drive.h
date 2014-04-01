/*  Copyright 2011 MaidSafe.net limited

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

#ifndef MAIDSAFE_DRIVE_DRIVE_H_
#define MAIDSAFE_DRIVE_DRIVE_H_

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"
#include "boost/thread/future.hpp"

#include "maidsafe/common/asio_service.h"
#include "maidsafe/common/rsa.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/drive/config.h"
#include "maidsafe/drive/meta_data.h"
#include "maidsafe/drive/directory_handler.h"
#include "maidsafe/drive/utils.h"
#include "maidsafe/drive/tools/launcher.h"

namespace maidsafe {

namespace drive {

template <typename Storage>
class Drive {
 public:
  Identity root_parent_id() const;
  boost::future<void> GetMountFuture();

 protected:
  Drive(std::shared_ptr<Storage> storage, const Identity& unique_user_id,
        const Identity& root_parent_id, const boost::filesystem::path& mount_dir,
        const boost::filesystem::path& user_app_dir,
        const std::string& mount_status_shared_object_name, bool create);

  virtual ~Drive();
  virtual void Mount() = 0;
  virtual void Unmount() = 0;

  const detail::FileContext* GetContext(const boost::filesystem::path& relative_path);
  detail::FileContext* GetMutableContext(const boost::filesystem::path& relative_path);
  void Create(const boost::filesystem::path& relative_path, detail::FileContext&& file_context);
  void Open(const boost::filesystem::path& relative_path);
  void Flush(const boost::filesystem::path& relative_path);
  void Release(const boost::filesystem::path& relative_path);
  void ReleaseDir(const boost::filesystem::path& relative_path);
  void Delete(const boost::filesystem::path& relative_path);
  void Rename(const boost::filesystem::path& old_relative_path,
              const boost::filesystem::path& new_relative_path);
  uint32_t Read(const boost::filesystem::path& relative_path, char* data, uint32_t size,
                uint64_t offset);
  uint32_t Write(const boost::filesystem::path& relative_path, const char* data, uint32_t size,
                 uint64_t offset);

  std::shared_ptr<Storage> storage_;
  const boost::filesystem::path kMountDir_;
  const boost::filesystem::path kUserAppDir_;
  const std::string kMountStatusSharedObjectName_;
  boost::promise<void> mount_promise_;
  std::once_flag unmounted_once_flag_;

 private:
  void InitialiseEncryptor(detail::FileContext& file_context);
  void ScheduleDeletionOfEncryptor(detail::FileContext* file_context);

  std::function<NonEmptyString(const std::string&)> get_chunk_from_store_;
  std::function<void(const ImmutableData&)> put_chunk_to_store_;
  std::function<void(const std::string&)> delete_chunk_from_store_;

 protected:
  AsioService asio_service_;
  // Needs to be destructed first so that 'get_chunk_from_store_' and 'storage_' outlive it.
  detail::DirectoryHandler<Storage> directory_handler_;
};

// ==================== Implementation =============================================================
template <typename Storage>
Drive<Storage>::Drive(std::shared_ptr<Storage> storage, const Identity& unique_user_id,
                      const Identity& root_parent_id, const boost::filesystem::path& mount_dir,
                      const boost::filesystem::path& user_app_dir,
                      const std::string& mount_status_shared_object_name, bool create)
    : storage_(storage),
      kMountDir_(mount_dir),
      kUserAppDir_(user_app_dir),
      kMountStatusSharedObjectName_(mount_status_shared_object_name),
      mount_promise_(),
      unmounted_once_flag_(),
      get_chunk_from_store_([this](const std::string& name)->NonEmptyString {
          try {
            auto chunk(storage_->Get(ImmutableData::Name(Identity(name))).get());
            return chunk.data();
          }
          catch (const std::exception& e) {
            LOG(kError) << "Failed to get " << name << " from storage: " << e.what();
            throw;
          }
        }),
      put_chunk_to_store_([this](const ImmutableData& chunk)->void {
          try {
            storage_->Put(chunk);
          }
          catch(const std::exception& e) {
            LOG(kError) << "Failed to put " << chunk.name().value.string() << " to storage: "
                        << e.what();
            throw;
          }
        }),
      delete_chunk_from_store_([this](const std::string& name)->void {
          try {
            storage_->Delete(ImmutableData::Name(Identity(name)));
          }
          catch (const std::exception& e) {
            LOG(kError) << "Failed to delete " << name << " from storage: " << e.what();
            throw;
          }
        }),
      asio_service_(2),
      directory_handler_(storage, unique_user_id, root_parent_id, create, get_chunk_from_store_,
                         put_chunk_to_store_, delete_chunk_from_store_, asio_service_.service()) {}

template <typename Storage>
Drive<Storage>::~Drive() {
  asio_service_.Stop();
}

template <typename Storage>
Identity Drive<Storage>::root_parent_id() const {
  return directory_handler_.root_parent_id();
}

template <typename Storage>
boost::future<void> Drive<Storage>::GetMountFuture() {
  return mount_promise_.get_future();
}

template <typename Storage>
void Drive<Storage>::InitialiseEncryptor(detail::FileContext& file_context) {
  assert(*file_context.open_count == 0 || *file_context.open_count == 1);
  if (!file_context.timer) {
    file_context.timer.reset(new boost::asio::steady_timer(asio_service_.service()));
  } else if (file_context.timer->cancel() > 0) {
    // Encryptor was about to to be deleted
    assert(file_context.self_encryptor);
    return;
  }
  file_context.self_encryptor.reset(new encrypt::SelfEncryptor(*file_context.meta_data.data_map,
      get_chunk_from_store_, put_chunk_to_store_, delete_chunk_from_store_));
}

template <typename Storage>
void Drive<Storage>::ScheduleDeletionOfEncryptor(detail::FileContext* file_context) {
  auto cancelled_count(file_context->timer->expires_from_now(detail::kFileInactivityDelay));
#ifndef NDEBUG
  if (cancelled_count > 0) {
    LOG(kInfo) << "Successfully cancelled " << cancelled_count << " encryptor deletion.";
    assert(cancelled_count == 1);
  }
  auto name(file_context->meta_data.name);
#endif
  static_cast<void>(cancelled_count);
  file_context->timer->async_wait([=](const boost::system::error_code& ec) {
      if (ec != boost::asio::error::operation_aborted) {
        if (*file_context->open_count == 0) {
#ifndef NDEBUG
          LOG(kInfo) << "Deleting encryptor for " << name;
#endif
          file_context->parent->FlushChildAndDeleteEncryptor(file_context);
        } else {
          LOG(kWarning) << "About to delete encryptor for " << name << " but open_count > 0";
        }
      } else {
#ifndef NDEBUG
        LOG(kSuccess) << "Timer was cancelled - not deleting encryptor for " << name;
#endif
      }
  });
}

template <typename Storage>
const detail::FileContext* Drive<Storage>::GetContext(
    const boost::filesystem::path& relative_path) {
  detail::Directory* parent(directory_handler_.Get(relative_path.parent_path()));
  return parent->GetChild(relative_path.filename());
}

template <typename Storage>
detail::FileContext* Drive<Storage>::GetMutableContext(
    const boost::filesystem::path& relative_path) {
  SCOPED_PROFILE
  detail::Directory* parent(directory_handler_.Get(relative_path.parent_path()));
  return parent->GetMutableChild(relative_path.filename());
}

template <typename Storage>
void Drive<Storage>::Create(const boost::filesystem::path& relative_path,
                            detail::FileContext&& file_context) {
  if (!file_context.meta_data.directory_id) {
    InitialiseEncryptor(file_context);
    *file_context.open_count = 1;
  }
  directory_handler_.Add(relative_path, std::move(file_context));
}

template <typename Storage>
void Drive<Storage>::Open(const boost::filesystem::path& relative_path) {
  detail::Directory* parent(directory_handler_.Get(relative_path.parent_path()));
  auto file_context(parent->GetMutableChild(relative_path.filename()));
  if (!file_context->meta_data.directory_id) {
    LOG(kInfo) << "Opening " << relative_path << " open count: " << *file_context->open_count + 1;
    if (++(*file_context->open_count) == 1) {
      std::lock_guard<std::mutex> lock(parent->mutex_);
      InitialiseEncryptor(*file_context);
    }
  }
}

template <typename Storage>
void Drive<Storage>::Flush(const boost::filesystem::path& relative_path) {
  auto file_context(GetMutableContext(relative_path));
  if (file_context->self_encryptor && !file_context->self_encryptor->Flush()) {
    LOG(kError) << "Failed to flush " << relative_path;
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::unknown));
  }
}

template <typename Storage>
void Drive<Storage>::Release(const boost::filesystem::path& relative_path) {
  SCOPED_PROFILE
  auto file_context(GetMutableContext(relative_path));
  if (!file_context->meta_data.directory_id) {
    LOG(kInfo) << "Releasing " << relative_path << " open count: " << *file_context->open_count - 1;
    --(*file_context->open_count);
    if (*file_context->open_count == 0)
      ScheduleDeletionOfEncryptor(file_context);
  }
}

template <typename Storage>
void Drive<Storage>::ReleaseDir(const boost::filesystem::path& relative_path) {
  SCOPED_PROFILE
  auto directory(directory_handler_.Get(relative_path));
  directory->ResetChildrenCounter();
}

template <typename Storage>
void Drive<Storage>::Delete(const boost::filesystem::path& relative_path) {
  directory_handler_.Delete(relative_path);
}

template <typename Storage>
void Drive<Storage>::Rename(const boost::filesystem::path& old_relative_path,
                            const boost::filesystem::path& new_relative_path) {
  directory_handler_.Rename(old_relative_path, new_relative_path);
}

template <typename Storage>
uint32_t Drive<Storage>::Read(const boost::filesystem::path& relative_path, char* data,
                              uint32_t size, uint64_t offset) {
  auto file_context(GetContext(relative_path));
  assert(file_context->self_encryptor);
  LOG(kInfo) << "For "  << relative_path << ", reading " << size << " of "
             << file_context->self_encryptor->size() << " bytes at offset " << offset;
  if (!file_context->self_encryptor->Read(data, size, offset))
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::unknown));
  // TODO(Fraser#5#): 2013-12-02 - Update last access time?
  if (offset + size > file_context->self_encryptor->size()) {
    return offset > file_context->self_encryptor->size() ? 0 :
           static_cast<uint32_t>(file_context->self_encryptor->size() - offset);
  } else {
    return size;
  }
}

template <typename Storage>
uint32_t Drive<Storage>::Write(const boost::filesystem::path& relative_path, const char* data,
                               uint32_t size, uint64_t offset) {
  auto file_context(GetMutableContext(relative_path));
  assert(file_context->self_encryptor);
  LOG(kInfo) << "For "  << relative_path << ", writing " << size << " bytes at offset " << offset;
  if (!file_context->self_encryptor->Write(data, size, offset))
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::unknown));
  // TODO(Fraser#5#): 2013-12-02 - Update last write time?
#ifndef MAIDSAFE_WIN32
  int64_t max_size(
      std::max(static_cast<off_t>(offset + size), file_context->meta_data.attributes.st_size));
  file_context->meta_data.attributes.st_size = max_size;
  file_context->meta_data.attributes.st_blocks = file_context->meta_data.attributes.st_size / 512;
#endif
  file_context->parent->ScheduleForStoring();
  return size;
}

}  // namespace drive

}  // namespace maidsafe

#endif  // MAIDSAFE_DRIVE_DRIVE_H_
