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

#include "maidsafe/drive/directory.h"

#include <algorithm>
#include <iterator>

#include "maidsafe/common/profiler.h"

#include "maidsafe/drive/meta_data.h"
#include "maidsafe/drive/utils.h"
#include "maidsafe/drive/proto_structs.pb.h"

namespace fs = boost::filesystem;

namespace maidsafe {

namespace drive {

namespace detail {

namespace {

std::function<void(const boost::system::error_code&)> GetStoreFunctor(
    Directory* directory, std::function<void(Directory*)> put_functor,  // NOLINT
    const boost::filesystem::path& path) {
  return [=](const boost::system::error_code& ec) {  // NOLINT
    if (ec != boost::asio::error::operation_aborted) {
      LOG(kInfo) << "Storing " << path;
      put_functor(directory);
    } else {
      LOG(kInfo) << "Timer was cancelled - not storing " << path;
    }
  };
}

void FlushEncryptor(FileContext* file_context) {
  file_context->self_encryptor->Flush();
  if (file_context->open_count == 0)
    file_context->self_encryptor.reset();
  file_context->flushed = true;
}

}  // unnamed namespace

Directory::Directory(
    ParentId parent_id, DirectoryId directory_id, boost::asio::io_service& io_service,
    std::function<void(Directory*)> put_functor,  // NOLINT
    const boost::filesystem::path& path)
        : mutex_(), cond_var_(), parent_id_(std::move(parent_id)),
          directory_id_(std::move(directory_id)), timer_(io_service),
          store_functor_(GetStoreFunctor(this, put_functor, path)),
          versions_(), max_versions_(kMaxVersions), children_(), children_count_position_(0),
          store_state_(StoreState::kComplete) {
  DoScheduleForStoring();
}

Directory::Directory(
    ParentId parent_id, const std::string& serialised_directory,
    const std::vector<StructuredDataVersions::VersionName>& versions,
    boost::asio::io_service& io_service,
    std::function<void(Directory*)> put_functor,  // NOLINT
    const boost::filesystem::path& path)
        : mutex_(), cond_var_(), parent_id_(std::move(parent_id)), directory_id_(),
          timer_(io_service), store_functor_(GetStoreFunctor(this, put_functor, path)),
          versions_(std::begin(versions), std::end(versions)), max_versions_(kMaxVersions),
          children_(), children_count_position_(0), store_state_(StoreState::kComplete) {
  protobuf::Directory proto_directory;
  if (!proto_directory.ParseFromString(serialised_directory))
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::parsing_error));

  directory_id_ = Identity(proto_directory.directory_id());
  max_versions_ = MaxVersions(proto_directory.max_versions());

  for (int i(0); i != proto_directory.children_size(); ++i)
    children_.emplace_back(new FileContext(MetaData(proto_directory.children(i)), this));
  SortAndResetChildrenCounter();
}

Directory::~Directory() {
  std::unique_lock<std::mutex> lock(mutex_);
  DoScheduleForStoring(false);
  bool result(cond_var_.wait_for(lock, kDirectoryInactivityDelay + std::chrono::milliseconds(500),
                                 [&] { return store_state_ == StoreState::kComplete; }));
  assert(result);
  static_cast<void>(result);
}

std::string Directory::Serialise() {
  protobuf::Directory proto_directory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    proto_directory.set_directory_id(directory_id_.string());
    proto_directory.set_max_versions(max_versions_.data);

    for (const auto& child : children_) {
      child->meta_data.ToProtobuf(proto_directory.add_children());
      if (child->self_encryptor) {
        child->timer->cancel();
        FlushEncryptor(child.get());
        child->flushed = false;
      }
    }
    store_state_ = StoreState::kOngoing;
  }
  return proto_directory.SerializeAsString();
}

void Directory::FlushChildAndDeleteEncryptor(FileContext* child) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (child->self_encryptor)  // Child could already have been flushed via 'Directory::Serialise'
    FlushEncryptor(child);
}

std::tuple<DirectoryId, StructuredDataVersions::VersionName, StructuredDataVersions::VersionName>
    Directory::AddNewVersion(ImmutableData::Name version_id) {
  std::tuple<DirectoryId, StructuredDataVersions::VersionName,
             StructuredDataVersions::VersionName> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    store_state_ = StoreState::kComplete;
    if (versions_.empty()) {
      versions_.emplace_back(0, version_id);
      result = std::make_tuple(directory_id_, StructuredDataVersions::VersionName(), versions_[0]);
    } else {
      versions_.emplace_front(versions_.front().index + 1, version_id);
      auto itr(std::begin(versions_));
      result = std::make_tuple(directory_id_, *(itr + 1), *itr);
    }
  }
  cond_var_.notify_one();
  return result;
}

Directory::Children::iterator Directory::Find(const fs::path& name) {
  return std::find_if(std::begin(children_), std::end(children_),
                      [&name](const Children::value_type& file_context) {
                           return file_context->meta_data.name == name; });
}

Directory::Children::const_iterator Directory::Find(const fs::path& name) const {
  return std::find_if(std::begin(children_), std::end(children_),
                      [&name](const Children::value_type& file_context) {
                           return file_context->meta_data.name == name; });
}

void Directory::SortAndResetChildrenCounter() {
  std::sort(std::begin(children_), std::end(children_),
            [](const std::unique_ptr<FileContext>& lhs, const std::unique_ptr<FileContext>& rhs) {
              return *lhs < *rhs;
            });
  children_count_position_ = 0;
}

void Directory::DoScheduleForStoring(bool use_delay) {
  if (use_delay) {
    auto cancelled_count(timer_.expires_from_now(kDirectoryInactivityDelay));
#ifndef NDEBUG
    if (cancelled_count > 0 && store_state_ != StoreState::kComplete) {
      LOG(kInfo) << "Successfully cancelled " << cancelled_count << " store functor.";
      assert(cancelled_count == 1);
    } else if (store_state_ != StoreState::kComplete) {
      LOG(kWarning) << "Failed to cancel store functor.";
    }
#endif
    static_cast<void>(cancelled_count);
    timer_.async_wait(store_functor_);
    store_state_ = StoreState::kPending;
  } else if (store_state_ == StoreState::kPending) {
    // If 'use_delay' is false, the implication is that we should only store if there's already
    // a pending store waiting - i.e. we're just bringing forward the deadline of any outstanding
    // store.
    auto cancelled_count(timer_.cancel());
    if (cancelled_count > 0) {
      LOG(kInfo) << "Successfully brought forward schedule for " << cancelled_count
                 << " store functor.";
      assert(cancelled_count == 1);
      timer_.get_io_service().post([this] { store_functor_(boost::system::error_code()); });
    } else {
      LOG(kWarning) << "Failed to cancel store functor.";
    }
    store_state_ = StoreState::kPending;
#ifndef NDEBUG
  } else {
    LOG(kInfo) << "No store functor pending.";
#endif
  }
}

bool Directory::HasChild(const fs::path& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(std::begin(children_), std::end(children_),
      [&name](const Children::value_type& file_context) {
          return file_context->meta_data.name == name; });
}

const FileContext* Directory::GetChild(const fs::path& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr(Find(name));
  if (itr == std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::no_such_file));
  // The open_count must be >=0.  If > 0 and the context doesn't represent a directory, the buffer
  // and encryptor should be non-null.
  assert(*(*itr)->open_count == 0 || (*(*itr)->open_count > 0 &&
      ((*itr)->meta_data.directory_id || ((*itr)->self_encryptor && (*itr)->timer))));
  return itr->get();
}

FileContext* Directory::GetMutableChild(const fs::path& name) {
  SCOPED_PROFILE
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr(Find(name));
  if (itr == std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::no_such_file));
  // The open_count must be >=0.  If > 0 and the context doesn't represent a directory, the buffer
  // and encryptor should be non-null.
  assert(*(*itr)->open_count == 0 || (*(*itr)->open_count > 0 &&
      ((*itr)->meta_data.directory_id || ((*itr)->self_encryptor && (*itr)->timer))));
  return itr->get();
}

const FileContext* Directory::GetChildAndIncrementCounter() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (children_count_position_ < children_.size()) {
    const FileContext* file_context(children_[children_count_position_].get());
    ++children_count_position_;
    return file_context;
  }
  return nullptr;
}

void Directory::AddChild(FileContext&& child) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr(Find(child.meta_data.name));
  if (itr != std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::file_exists));
  child.parent = this;
  children_.emplace_back(new FileContext(std::move(child)));
  SortAndResetChildrenCounter();
  DoScheduleForStoring();
}

FileContext Directory::RemoveChild(const fs::path& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto itr(Find(name));
  if (itr == std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::no_such_file));
  std::unique_ptr<FileContext> file_context(std::move(*itr));
  children_.erase(itr);
  SortAndResetChildrenCounter();
  DoScheduleForStoring();
  return std::move(*file_context);
}

void Directory::RenameChild(const fs::path& old_name, const fs::path& new_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(Find(new_name) == std::end(children_));
  auto itr(Find(old_name));
  if (itr == std::end(children_))
    BOOST_THROW_EXCEPTION(MakeError(DriveErrors::no_such_file));
  (*itr)->meta_data.name = new_name;
  SortAndResetChildrenCounter();
  DoScheduleForStoring();
}

void Directory::ResetChildrenCounter() {
  std::lock_guard<std::mutex> lock(mutex_);
  children_count_position_ = 0;
}

bool Directory::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return children_.empty();
}

ParentId Directory::parent_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return parent_id_;
}

void Directory::SetNewParent(const ParentId parent_id, std::function<void(Directory*)> put_functor,  // NOLINT
                             const boost::filesystem::path& path) {
  std::unique_lock<std::mutex> lock(mutex_);
  bool result(cond_var_.wait_for(lock, std::chrono::milliseconds(500),
                                 [&] { return store_state_ != StoreState::kOngoing; }));
  assert(result);
  static_cast<void>(result);
  parent_id_ = parent_id;
  store_functor_ = GetStoreFunctor(this, put_functor, path);
}

DirectoryId Directory::directory_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return directory_id_;
}

void Directory::ScheduleForStoring() {
  std::lock_guard<std::mutex> lock(mutex_);
  DoScheduleForStoring();
}

void Directory::StoreImmediatelyIfPending() {
  std::lock_guard<std::mutex> lock(mutex_);
  DoScheduleForStoring(false);
}

bool operator<(const Directory& lhs, const Directory& rhs) {
  return lhs.directory_id() < rhs.directory_id();
}

}  // namespace detail

}  // namespace drive

}  // namespace maidsafe
