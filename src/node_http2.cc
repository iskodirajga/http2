#include "node.h"
#include "node_buffer.h"
#include "nghttp2/nghttp2.h"
#include "node_http2.h"

#include "async-wrap.h"
#include "async-wrap-inl.h"
#include "env.h"
#include "env-inl.h"
#include "util.h"
#include "util-inl.h"
#include "v8.h"

#include <vector>

namespace node {

using v8::Array;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Handle;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Name;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::PropertyCallbackInfo;
using v8::String;
using v8::Value;

namespace http2 {

// Utilities

inline int32_t GetStreamID(Local<Value> val) {
  int32_t id = 0;
  if (val->IsObject()) {
    Http2Stream* stream;
    stream = Unwrap<Http2Stream>(val.As<Object>());
    if (stream != nullptr)
      id = stream->id();
  } else {
    id = val->Int32Value();
  }
  return id;
}


inline void GetHeaders(Local<Value> obj, std::vector<nghttp2_nv>* vector) {
  if (obj->IsArray()) {
    Local<Array> headers = obj.As<Array>();
    for (size_t i = 0; i < headers->Length(); i++) {
      Local<Value> val = headers->Get(i);
      if (val->IsObject()) {
        Http2Header* header;
        ASSIGN_OR_RETURN_UNWRAP(&header, val.As<Object>());
        vector->push_back(**header);
      }
    }
  }
}

// Http2Options statics

#define OPTIONS(obj, V)                                                \
  V(obj, "maxDeflateDynamicTableSize", SetMaxDeflateDynamicTableSize, Uint32) \
  V(obj, "maxReservedRemoteStreams", SetMaxReservedRemoteStreams, Uint32)     \
  V(obj, "maxSendHeaderBlockLength", SetMaxSendHeaderBlockLength, Uint32)     \
  V(obj, "peerMaxConcurrentStreams", SetPeerMaxConcurrentStreams, Uint32)     \
  V(obj, "noHttpMessaging", SetNoHttpMessaging, Boolean)                      \
  V(obj, "noRecvClientMagic", SetNoRecvClientMagic, Boolean)

Http2Options::Http2Options(Environment* env, Local<Value> options) {
  nghttp2_option_new(&options_);
  if (options->IsObject()) {
    Local<Object> opts = options.As<Object>();

#define V(obj, name, fn, type)                                                \
  {                                                                           \
    Local<Value> val = obj->Get(FIXED_ONE_BYTE_STRING(env->isolate(), name)); \
    if (!val.IsEmpty()) fn(val->type##Value());                               \
  }
    OPTIONS(opts, V)
#undef V
  }
}
#undef OPTIONS

// Http2Settings statics

typedef uint32_t(*get_setting)(nghttp2_session* session,
                               nghttp2_settings_id id);
Http2Settings::Http2Settings(Environment* env,
                             Local<Object> wrap,
                             Http2Session* session,
                             bool localSettings) :
                             BaseObject(env, wrap) {
  Wrap(object(), this);

  if (session != nullptr) {
    get_setting fn =
        localSettings ?
            nghttp2_session_get_local_settings :
            nghttp2_session_get_remote_settings;
      Set(NGHTTP2_SETTINGS_HEADER_TABLE_SIZE,
          fn(**session, NGHTTP2_SETTINGS_HEADER_TABLE_SIZE));
      Set(NGHTTP2_SETTINGS_ENABLE_PUSH,
          fn(**session, NGHTTP2_SETTINGS_ENABLE_PUSH));
      Set(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
          fn(**session, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS));
      Set(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
          fn(**session, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE));
      Set(NGHTTP2_SETTINGS_MAX_FRAME_SIZE,
          fn(**session, NGHTTP2_SETTINGS_MAX_FRAME_SIZE));
      Set(NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
          fn(**session, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE));
  }
}

void Http2Settings::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  if (!args.IsConstructCall())
    return env->ThrowTypeError("Class constructor Http2Settings cannot "
                               "be invoked without 'new'");
  new Http2Settings(env, args.This());
}

void Http2Settings::Defaults(const FunctionCallbackInfo<Value>& args) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, args.Holder());
  settings->settings_.clear();
  settings->Set(NGHTTP2_SETTINGS_HEADER_TABLE_SIZE,
                DEFAULT_SETTINGS_HEADER_TABLE_SIZE);
  settings->Set(NGHTTP2_SETTINGS_ENABLE_PUSH,
                DEFAULT_SETTINGS_ENABLE_PUSH);
  settings->Set(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
                DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE);
  settings->Set(NGHTTP2_SETTINGS_MAX_FRAME_SIZE,
                DEFAULT_SETTINGS_MAX_FRAME_SIZE);
}

void Http2Settings::Reset(const FunctionCallbackInfo<Value>& args) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, args.Holder());
  settings->settings_.clear();
}

void Http2Settings::Pack(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  HandleScope scope(env->isolate());
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, args.Holder());
  std::vector<nghttp2_settings_entry> entries;
  settings->CollectSettings(&entries);
  size_t len = entries.size() * 6;
  MaybeStackBuffer<char> buf(len);
  ssize_t ret =
      nghttp2_pack_settings_payload(
        reinterpret_cast<uint8_t*>(*buf), len, &entries[0], entries.size());
  if (ret >= 0) {
    args.GetReturnValue().Set(
      Buffer::Copy(env, *buf, len).ToLocalChecked());
  }
}

void Http2Settings::GetHeaderTableSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  settings->Find(NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, info);
}

void Http2Settings::SetHeaderTableSize(
    Local<String> property,
    Local<Value> value,
    const PropertyCallbackInfo<void>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  if (value->IsUndefined())
    settings->Erase(NGHTTP2_SETTINGS_HEADER_TABLE_SIZE);
  else
    settings->Set(NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, value->Uint32Value());
}

void Http2Settings::GetEnablePush(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  settings->FindBoolean(NGHTTP2_SETTINGS_ENABLE_PUSH, info);
}

void Http2Settings::SetEnablePush(
    Local<String> property,
    Local<Value> value,
    const PropertyCallbackInfo<void>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  if (value->IsUndefined())
    settings->Erase(NGHTTP2_SETTINGS_ENABLE_PUSH);
  else
    settings->Set(NGHTTP2_SETTINGS_ENABLE_PUSH, value->BooleanValue() ? 1 : 0);
}

void Http2Settings::GetMaxConcurrentStreams(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  settings->Find(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, info);
}

void Http2Settings::SetMaxConcurrentStreams(
    Local<String> property,
    Local<Value> value,
    const PropertyCallbackInfo<void>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  if (value->IsUndefined()) {
    settings->Erase(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
  } else {
    settings->Set(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
                  value->Uint32Value());
  }
}

void Http2Settings::GetInitialWindowSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  settings->Find(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, info);
}

void Http2Settings::SetInitialWindowSize(
    Local<String> property,
    Local<Value> value,
    const PropertyCallbackInfo<void>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  if (value->IsUndefined())
    settings->Erase(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE);
  else
    settings->Set(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
                  MIN(MAX_INITIAL_WINDOW_SIZE, value->Uint32Value()));
}

void Http2Settings::GetMaxFrameSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  settings->Find(NGHTTP2_SETTINGS_MAX_FRAME_SIZE, info);
}

void Http2Settings::SetMaxFrameSize(
    Local<String> property,
    Local<Value> value,
    const PropertyCallbackInfo<void>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  if (value->IsUndefined()) {
    settings->Erase(NGHTTP2_SETTINGS_MAX_FRAME_SIZE);
  } else {
    settings->Set(
        NGHTTP2_SETTINGS_MAX_FRAME_SIZE,
        MAX(MIN(value->Uint32Value(), MAX_MAX_FRAME_SIZE), MIN_MAX_FRAME_SIZE));
  }
}

void Http2Settings::GetMaxHeaderListSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  settings->Find(NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, info);
}
void Http2Settings::SetMaxHeaderListSize(
    Local<String> property,
    Local<Value> value,
    const PropertyCallbackInfo<void>& info) {
  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, info.Holder());
  if (value->IsUndefined())
    settings->Erase(NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE);
  else
    settings->Set(NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, value->Uint32Value());
}

// Http2Priority statics

// The Http2Priority class wraps the nghttp2_priority_spec struct.
Http2Priority::Http2Priority(int32_t parent, int32_t weight, bool exclusive) {
  if (weight < 0) weight = NGHTTP2_DEFAULT_WEIGHT;
  weight = MAX(MIN(weight, NGHTTP2_MAX_WEIGHT), NGHTTP2_MIN_WEIGHT);
  nghttp2_priority_spec_init(&spec_, parent, weight, exclusive ? 1 : 0);
}


// Http2DataProvider statics

Http2DataProvider::Http2DataProvider(Environment* env,
                                     Local<Object> wrap,
                                     Http2Stream* stream) :
                                     BaseObject(env, wrap),
                                     stream_(stream) {
  Wrap(object(), this);
  provider_.read_callback = on_read;
  provider_.source.ptr = this;
}

void Http2DataProvider::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  if (!args.IsConstructCall())
    return env->ThrowTypeError("Class constructor Http2DataProvider cannot "
                               "be invoked without 'new'");
  if (args.Length() < 1)
    return env->ThrowTypeError("'stream' argument is required");
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args[0].As<Object>());
  new Http2DataProvider(env, args.This(), stream);
}

ssize_t Http2DataProvider::on_read(nghttp2_session* session,
                                   int32_t stream_id,
                                   uint8_t* buf,
                                   size_t length,
                                   uint32_t* flags,
                                   nghttp2_data_source* source,
                                   void* user_data) {
  Http2DataProvider* provider =
    reinterpret_cast<Http2DataProvider*>(source->ptr);
  Http2Stream* stream = provider->stream();
  Local<Object> provider_obj = provider->object();
  Local<Object> stream_obj = stream->object();
  Environment* env = stream->env();
  Isolate* isolate = env->isolate();

  Local<Value> cb = provider_obj->Get(FIXED_ONE_BYTE_STRING(isolate, "_read"));
  CHECK(cb->IsFunction());

  Local<Object> retFlags = Object::New(isolate);

  Local<Object> buffer =
      Buffer::New(env, reinterpret_cast<char*>(buf), length,
                  &FreeCallbackNonop, nullptr).ToLocalChecked();
  Local<Value> argv[] {
    buffer,
    retFlags
  };
  Environment::AsyncCallbackScope callback_scope(env);
  v8::MaybeLocal<Value> ret = cb.As<Function>()->Call(env->context(),
                                             stream_obj,
                                             arraysize(argv),
                                             argv);
  CHECK(!ret.IsEmpty());
  int32_t val = ret.ToLocalChecked()->Int32Value();

  // TODO(jasnell): There's likely a better, more elegant way of doing this.
  if (retFlags->Get(FLAG_ENDSTREAM)->BooleanValue())
    *flags |= NGHTTP2_FLAG_END_STREAM;
  if (retFlags->Get(FLAG_ENDDATA)->BooleanValue())
    *flags |= NGHTTP2_DATA_FLAG_EOF;
  if (retFlags->Get(FLAG_NOENDSTREAM)->BooleanValue())
    *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;

  return val;
}

// Http2Header statics

// The Http2Header class wraps an individual nghttp2_nv struct.
Http2Header::Http2Header(Environment* env,
                         Local<Object> wrap,
                         char* name, size_t nlen,
                         char* value, size_t vlen) :
                         BaseObject(env, wrap) {
  Wrap(object(), this);
  // The following uses a single MaybeStackBuffer to store both the
  // header key and value, setting pointers to the start of each value
  // within that single buffer.
  store_.AllocateSufficientStorage(nlen + vlen);
  nv_.name = *store_;
  nv_.value = *store_ + nlen;
  nv_.namelen = nlen;
  nv_.valuelen = vlen;
  nv_.flags = NGHTTP2_NV_FLAG_NONE;
  memcpy(*store_, name, nlen);
  memcpy(*store_ + nlen, value, vlen);
}

void Http2Header::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  if (!args.IsConstructCall())
    return env->ThrowTypeError("Class constructor Http2Header cannot "
                               "be invoked without 'new'");
  if (!args[0]->IsString())
    return env->ThrowTypeError("First argument must be a string");
  if (!args[1]->IsString())
    return env->ThrowTypeError("Second argument must be a string");
  Utf8Value key(isolate, args[0].As<String>());
  Utf8Value value(isolate, args[1].As<String>());
  if (key.length() == 0)
    return env->ThrowTypeError("First argument must not be an empty string");
  new Http2Header(env, args.This(), *key, key.length(), *value, value.length());
}


void Http2Header::GetName(Local<String> property,
                          const PropertyCallbackInfo<Value>& args) {
  Http2Header* header;
  ASSIGN_OR_RETURN_UNWRAP(&header, args.Holder());
  Environment* env = header->env();
  args.GetReturnValue().Set(String::NewFromUtf8(env->isolate(),
                              reinterpret_cast<const char*>((**header).name),
                              v8::NewStringType::kNormal,
                              (**header).namelen).ToLocalChecked());
}


void Http2Header::GetValue(Local<String> property,
                           const PropertyCallbackInfo<Value>& args) {
  Http2Header* header;
  ASSIGN_OR_RETURN_UNWRAP(&header, args.Holder());
  Environment* env = header->env();
  args.GetReturnValue().Set(
      String::NewFromUtf8(env->isolate(),
                          reinterpret_cast<const char*>((**header).value),
                          v8::NewStringType::kNormal,
                          (**header).valuelen).ToLocalChecked());
}


void Http2Header::GetFlags(Local<String> property,
                           const PropertyCallbackInfo<Value>& info) {
  Http2Header* header;
  ASSIGN_OR_RETURN_UNWRAP(&header, info.Holder());
  info.GetReturnValue().Set(header->nv_.flags);
}

void Http2Header::SetFlags(Local<String> property,
                           Local<Value> value,
                           const PropertyCallbackInfo<void>& info) {
  Http2Header* header;
  ASSIGN_OR_RETURN_UNWRAP(&header, info.Holder());
  header->nv_.flags = value->Uint32Value();
}

// Http2Stream Statics

// The Http2Stream class wraps an individual nghttp2_stream struct.
Http2Stream::Http2Stream(Environment* env,
                         Local<Object> wrap,
                         Http2Session* session,
                         int32_t stream_id) :
                         AsyncWrap(env, wrap, AsyncWrap::PROVIDER_HTTP2STREAM),
                         session_(session),
                         stream_id_(stream_id) {
  Wrap(object(), this);
  prev_ = nullptr;
  next_ = nullptr;
  stream_ = nghttp2_session_find_stream(**session, stream_id);
}

// TODO(jasnell): Implement these. The Add Stream and Remove Stream methods
// are used as part of the HTTP/2 stream prioritization grouping.
void Http2Stream::RemoveStream(Http2Stream* stream) {}
void Http2Stream::AddStream(Http2Stream* stream, Http2Session* session) {}

void Http2Stream::GetUid(Local<String> property,
                         const PropertyCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Environment* env = stream->env();
  args.GetReturnValue().Set(Number::New(env->isolate(), stream->get_uid()));
}

void Http2Stream::GetID(Local<String> property,
                        const PropertyCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  args.GetReturnValue().Set(stream->id());
}

void Http2Stream::GetState(Local<String> property,
                           const PropertyCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  args.GetReturnValue().Set(nghttp2_stream_get_state(**stream));
}

void Http2Stream::GetSumDependencyWeight(
    Local<String> property,
    const PropertyCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  args.GetReturnValue().Set(nghttp2_stream_get_sum_dependency_weight(**stream));
}

void Http2Stream::GetWeight(Local<String> property,
                            const PropertyCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  args.GetReturnValue().Set(nghttp2_stream_get_weight(**stream));
}

void Http2Stream::GetLocalWindowSize(Local<String> property,
                                     const PropertyCallbackInfo<Value>& info) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, info.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(
      nghttp2_session_get_stream_local_window_size(**session, stream->id()));
}

void Http2Stream::SetLocalWindowSize(Local<String> property,
                                     Local<Value> value,
                                     const PropertyCallbackInfo<void>& info) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, info.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  nghttp2_session_set_local_window_size(
      **session, NGHTTP2_FLAG_NONE, stream->id(), value->Int32Value());
}

void Http2Stream::GetStreamLocalClose(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, info.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(
      nghttp2_session_get_stream_local_close(**session, stream->id()));
}

void Http2Stream::GetStreamRemoteClose(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, info.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(
      nghttp2_session_get_stream_remote_close(**session, stream->id()));
}


void Http2Stream::SendTrailers(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  std::vector<nghttp2_nv> headers;
  GetHeaders(args[0], &headers);
  args.GetReturnValue().Set(
      nghttp2_submit_trailer(**session, stream->id(),
                             &headers[0], headers.size()));
}

void Http2Stream::ResumeData(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  if (stream->IsLocalOpen()) {
    nghttp2_stream_get_state(**stream);
    args.GetReturnValue().Set(
        nghttp2_session_resume_data(**session, stream->id()));
  } else {
    args.GetReturnValue().Set(0);
  }
}

void Http2Stream::SendContinue(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  nghttp2_nv headers[] { MAKE_NV(HTTP2_HEADER_STATUS, "100") };
  args.GetReturnValue().Set(
      nghttp2_submit_headers(
          **session, NGHTTP2_FLAG_NONE, stream->id(),
          nullptr, &headers[0], 1, nullptr));
}

void Http2Stream::Respond(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  nghttp2_data_provider* provider = nullptr;
  std::vector<nghttp2_nv> headers;
  GetHeaders(args[0], &headers);
  if (args.Length() > 1) {
    if (!args[1]->IsObject())
      return env->ThrowTypeError(
        "Second argument must be an Http2DataProvider object");
    Http2DataProvider* dataProvider;
    ASSIGN_OR_RETURN_UNWRAP(&dataProvider, args[1].As<Object>());
    provider = **dataProvider;
  }
  args.GetReturnValue().Set(
      nghttp2_submit_response(
          **session, stream->id(), &headers[0], headers.size(), provider));
}

void Http2Stream::SendDataFrame(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);

  uint8_t flags = 0;
  if (args[0]->BooleanValue())
    flags |= NGHTTP2_FLAG_END_STREAM;

  if (!args[1]->IsObject())
    return env->ThrowTypeError(
      "Second argument must be an Http2DataProvider object");
  Http2DataProvider* provider;
  ASSIGN_OR_RETURN_UNWRAP(&provider, args[1].As<Object>());

  args.GetReturnValue().Set(
      nghttp2_submit_data(**session, flags, stream->id(), **provider));
}

void Http2Stream::SendRstStream(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  args.GetReturnValue().Set(
      nghttp2_submit_rst_stream(**session, NGHTTP2_FLAG_NONE,
                                stream->id(), args[0]->Uint32Value()));
}

void Http2Stream::SendPriority(const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  Http2Priority priority(args[0]->Int32Value(),
                         args[1]->Int32Value(),
                         args[2]->BooleanValue());
  args.GetReturnValue().Set(
      nghttp2_submit_priority(**session, NGHTTP2_FLAG_NONE,
                              stream->id(), *priority));
}

void Http2Stream::ChangeStreamPriority(
    const FunctionCallbackInfo<Value>& args) {
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  Http2Priority priority(args[0]->Int32Value(),
                         args[1]->Int32Value(),
                         args[2]->BooleanValue());
  args.GetReturnValue().Set(
      nghttp2_session_change_stream_priority(
          **session, stream->id(), *priority));
}

void Http2Stream::SendPushPromise(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  HandleScope scope(env->isolate());
  Http2Stream* stream;
  ASSIGN_OR_RETURN_UNWRAP(&stream, args.Holder());
  Http2Session* session = stream->session();
  SESSION_OR_RETURN(session);
  if (nghttp2_session_check_server_session(**session) == 0) {
    return env->ThrowError("Client Http2Session instances cannot use push");
  }
  std::vector<nghttp2_nv> headers;
  GetHeaders(args[0], &headers);
  int32_t ret =
      nghttp2_submit_push_promise(**session,
                                  NGHTTP2_FLAG_NONE,
                                  stream->id(),
                                  &headers[0], headers.size(),
                                  stream);
  if (ret > 0) {
    args.GetReturnValue().Set(
        Http2Session::create_stream(env, session, ret)->object());
  } else {
    args.GetReturnValue().Set(ret);
  }
}

// Http2Session Statics

// The Http2Session class wraps an individual nghttp2_session struct.
Http2Session::Http2Session(Environment* env,
                           Local<Object> wrap,
                           enum http2_session_type type,
                           Local<Value> options) :
                           AsyncWrap(env, wrap,
                                     AsyncWrap::PROVIDER_HTTP2SESSION),
                           type_(type) {
  Wrap(object(), this);
  nghttp2_session_callbacks* cb;
  nghttp2_session_callbacks_new(&cb);
  SET_SESSION_CALLBACK(cb, send)
  SET_SESSION_CALLBACK(cb, on_frame_recv)
  SET_SESSION_CALLBACK(cb, on_stream_close)
  SET_SESSION_CALLBACK(cb, on_header)
  SET_SESSION_CALLBACK(cb, on_begin_headers)
  SET_SESSION_CALLBACK(cb, on_data_chunk_recv)
  SET_SESSION_CALLBACK(cb, on_frame_send)
  SET_SESSION_CALLBACK(cb, select_padding);
  Http2Options opts(env, options);
  switch (type) {
    case SESSION_TYPE_CLIENT:
      nghttp2_session_client_new2(&session_, cb, this, *opts);
      break;
    case SESSION_TYPE_SERVER:
      // Fallthrough
    default:
      nghttp2_session_server_new2(&session_, cb, this, *opts);
      break;
  }
  nghttp2_session_callbacks_del(cb);
  root_ = create_stream(env, this, 0);
}

void Http2Session::GetUid(Local<String> property,
                          const PropertyCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  Environment* env = session->env();
  args.GetReturnValue().Set(Number::New(env->isolate(), session->get_uid()));
}

// The send callback is invoked by the nghttp library when there is outgoing
// data to be sent to a connected peer. The user_data is a pointer to the
// Http2Session wrapper.
ssize_t Http2Session::send(nghttp2_session* session,
                           const uint8_t* data,
                           size_t length,
                           int flags,
                           void *user_data) {
  Http2Session* session_obj =
    reinterpret_cast<Http2Session*>(user_data);
  Environment* env = session_obj->env();

  // Copy the data because we don't own it and cannot be sure
  // exactly when it will be released by the nghttp2 library.
  Local<Object> buffer =
      Buffer::Copy(env, reinterpret_cast<const char*>(data),
                   length).ToLocalChecked();
  EMIT(env, session_obj, "send", buffer);
  return length;
}

int Http2Session::on_rst_stream_frame(Http2Session* session,
                                      int32_t id,
                                      const nghttp2_frame_hd hd,
                                      const nghttp2_rst_stream rst) {
  Environment* env = session->env();
  EMIT(env, session, "rst-stream",
       Integer::New(env->isolate(), id),
       Integer::NewFromUnsigned(env->isolate(), rst.error_code));
  return 0;
}


int Http2Session::on_goaway_frame(Http2Session* session,
                                  const nghttp2_frame_hd hd,
                                  const nghttp2_goaway goaway) {
  Environment* env = session->env();
  Environment::AsyncCallbackScope callback_scope(env);
  Isolate* isolate = env->isolate();

  Local<Value> opaque_data;
  if (goaway.opaque_data_len > 0) {
    opaque_data =
        Buffer::Copy(env, reinterpret_cast<const char*>(goaway.opaque_data),
                     goaway.opaque_data_len).ToLocalChecked();
  } else {
    opaque_data = Undefined(isolate);
  }

  EMIT(env, session, "goaway",
       Integer::NewFromUnsigned(isolate, goaway.error_code),
       Integer::New(isolate, goaway.last_stream_id),
       opaque_data);

  return 0;
}


int Http2Session::on_data_frame(Http2Session* session,
                                Http2Stream* stream,
                                const nghttp2_frame_hd hd,
                                const nghttp2_data data) {
  Environment* env = session->env();
  Isolate* isolate = env->isolate();
  EMIT(env, session, "data",
       stream->object(),
       Integer::NewFromUnsigned(isolate, hd.flags),
       Integer::New(isolate, hd.length),
       Integer::New(isolate, data.padlen));
  return 0;
}

int Http2Session::on_data_chunk_recv(nghttp2_session* session,
                                     uint8_t flags,
                                     int32_t stream_id,
                                     const uint8_t* data,
                                     size_t len,
                                     void* user_data) {
  Http2Session* session_obj =
    reinterpret_cast<Http2Session*>(user_data);
  Environment* env = session_obj->env();
  Isolate* isolate = env->isolate();
  const char* cdata = reinterpret_cast<const char*>(data);
  Http2Stream* stream =
      static_cast<Http2Stream*>(
        nghttp2_session_get_stream_user_data(session, stream_id));
  EMIT(env, session_obj, "data-chunk",
       stream->object(),
       Integer::NewFromUnsigned(isolate, flags),
       Buffer::Copy(env, cdata, len).ToLocalChecked());
  return 0;
}

int Http2Session::on_headers_frame(Http2Session* session,
                                   Http2Stream* stream,
                                   const nghttp2_frame_hd hd,
                                   const nghttp2_headers headers) {
  Environment* env = session->env();
  EMIT(env, session, "headers-complete",
       stream->object(),
       Integer::NewFromUnsigned(env->isolate(), hd.flags));
  return 0;
}

// Called when nghttp2 receives a frame from the connected peer.
int Http2Session::on_frame_recv(nghttp2_session *session,
                                const nghttp2_frame *frame,
                                void *user_data) {
  Http2Session* session_obj =
    reinterpret_cast<Http2Session*>(user_data);
  Http2Stream* stream_data;
  // TODO(jasnell): This needs to handle the other frame types
  switch (frame->hd.type) {
  case NGHTTP2_RST_STREAM:
    return on_rst_stream_frame(session_obj,
                               frame->hd.stream_id,
                               frame->hd,
                               frame->rst_stream);
  case NGHTTP2_GOAWAY:
    return on_goaway_frame(session_obj, frame->hd, frame->goaway);
  case NGHTTP2_DATA:
    stream_data =
        reinterpret_cast<Http2Stream*>(
            nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    return on_data_frame(session_obj, stream_data, frame->hd, frame->data);
  case NGHTTP2_HEADERS:
    stream_data =
      reinterpret_cast<Http2Stream*>(
          nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    return on_headers_frame(session_obj, stream_data,
                            frame->hd, frame->headers);
  default:
    return 0;
  }
}


int Http2Session::on_stream_close(nghttp2_session *session,
                                  int32_t stream_id,
                                  uint32_t error_code,
                                  void *user_data) {
  Http2Session* session_obj =
    reinterpret_cast<Http2Session*>(user_data);
  Environment* env = session_obj->env();
  Environment::AsyncCallbackScope callback_scope(env);

  Http2Stream* stream_data =
      reinterpret_cast<Http2Stream*>(
        nghttp2_session_get_stream_user_data(session, stream_id));
  if (!stream_data)
    return 0;
  EMIT(env, session_obj, "stream-close",
       stream_data->object(),
       Integer::NewFromUnsigned(env->isolate(), error_code));

  return 0;
}

// Called when an individual header name+value pair is processed by nghttp2.
int Http2Session::on_header(nghttp2_session *session,
                            const nghttp2_frame *frame,
                            const uint8_t *name,
                            size_t namelen,
                            const uint8_t *value,
                            size_t valuelen,
                            uint8_t flags,
                            void *user_data) {
  Http2Session* session_obj =
    reinterpret_cast<Http2Session*>(user_data);
  Environment* env = session_obj->env();
  Isolate* isolate = env->isolate();

  Http2Stream* stream_data =
      reinterpret_cast<Http2Stream*>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
  CHECK(stream_data != nullptr);

  EMIT(env, session_obj, "header",
       stream_data->object(),
       OneByteString(isolate, name, namelen),
       OneByteString(isolate, value, valuelen));

  return 0;
}


int Http2Session::on_begin_headers(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data) {
  Http2Session* session_obj =
    reinterpret_cast<Http2Session*>(user_data);
  Environment* env = session_obj->env();

  Http2Stream* stream_data =
      reinterpret_cast<Http2Stream*>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
  if (stream_data == nullptr) {
    stream_data = create_stream(env, session_obj, frame->hd.stream_id);
  }

  EMIT(env, session_obj, "begin-headers",
       stream_data->object(),
       Integer::NewFromUnsigned(env->isolate(), frame->headers.cat));

  return 0;
}


// Called when nghttp2 sends a frame to the connected peer
int Http2Session::on_frame_send(nghttp2_session* session,
                                const nghttp2_frame* frame,
                                void* user_data) {
  Http2Session* session_obj =
    reinterpret_cast<Http2Session*>(user_data);
  Environment* env = session_obj->env();
  Isolate* isolate = env->isolate();
  EMIT(env, session_obj, "frame-sent",
       Integer::NewFromUnsigned(isolate, frame->hd.stream_id),
       Integer::NewFromUnsigned(isolate, frame->hd.type),
       Integer::NewFromUnsigned(isolate, frame->hd.flags));
  return 0;
}

ssize_t Http2Session::select_padding(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     size_t max_payloadlen,
                                     void *user_data) {
  // TODO(jasnell): Determine algorithm for seleting padding
  return frame->hd.length;
}

Http2Stream* Http2Session::create_stream(Environment* env,
                                         Http2Session* session,
                                         uint32_t stream_id) {
  CHECK_EQ(env->http2stream_constructor_template().IsEmpty(), false);
  Local<Function> constructor =
      env->http2stream_constructor_template()->GetFunction();
  CHECK_EQ(constructor.IsEmpty(), false);
  Local<Object> obj =
      constructor->NewInstance(env->context()).ToLocalChecked();
  Http2Stream* stream = new Http2Stream(env, obj, session, stream_id);
  if (stream_id > 0)
    Http2Stream::AddStream(stream, session);
  nghttp2_session_set_stream_user_data(**session, stream_id, stream);
  return stream;
}

void Http2Session::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  if (!args.IsConstructCall())
    return env->ThrowTypeError("Class constructor Http2Session cannot "
                               "be invoked without 'new'");
  enum http2_session_type type =
      static_cast<enum http2_session_type>(args[0]->Int32Value());
  if (type != SESSION_TYPE_SERVER && type != SESSION_TYPE_CLIENT)
    return env->ThrowTypeError("Invalid HTTP/2 session type");

  new Http2Session(env, args.This(), type, args[1]);
}


void Http2Session::GetWantRead(Local<String> property,
                               const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  info.GetReturnValue().Set(nghttp2_session_want_read(**session) != 0);
}

void Http2Session::GetWantWrite(Local<String> property,
                                const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  info.GetReturnValue().Set(nghttp2_session_want_write(**session) != 0);
}

void Http2Session::GetRootStream(Local<String> property,
                                 const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  info.GetReturnValue().Set(session->root_->object());
}


void Http2Session::GetType(Local<String> property,
                           const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  info.GetReturnValue().Set(session->type_);
}


void Http2Session::GetEffectiveLocalWindowSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(
      nghttp2_session_get_effective_local_window_size(**session));
}


void Http2Session::GetEffectiveRecvDataLength(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(
      nghttp2_session_get_effective_recv_data_length(**session));
}


void Http2Session::GetNextStreamID(Local<String> property,
                                   const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(nghttp2_session_get_next_stream_id(**session));
}


void Http2Session::SetNextStreamID(Local<String> property,
                                   Local<Value> value,
                                   const PropertyCallbackInfo<void>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  int32_t id = value->Int32Value();
  nghttp2_session_set_next_stream_id(**session, id);
}


void Http2Session::GetLocalWindowSize(Local<String> property,
                                   const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(nghttp2_session_get_local_window_size(**session));
}

void Http2Session::SetLocalWindowSize(Local<String> property,
                                      Local<Value> value,
                                      const PropertyCallbackInfo<void>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  nghttp2_session_set_local_window_size(
      **session, NGHTTP2_FLAG_NONE, 0, value->Int32Value());
}

void Http2Session::GetLastProcStreamID(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(nghttp2_session_get_last_proc_stream_id(**session));
}


void Http2Session::GetRemoteWindowSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  info.GetReturnValue().Set(nghttp2_session_get_remote_window_size(**session));
}


void Http2Session::GetOutboundQueueSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  size_t size = nghttp2_session_get_outbound_queue_size(**session);
  Environment* env = session->env();
  info.GetReturnValue().Set(Integer::New(env->isolate(), size));
}


void Http2Session::GetDeflateDynamicTableSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  size_t size = nghttp2_session_get_hd_deflate_dynamic_table_size(**session);
  Environment* env = session->env();
  info.GetReturnValue().Set(Integer::New(env->isolate(), size));
}


void Http2Session::GetInflateDynamicTableSize(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);
  size_t size = nghttp2_session_get_hd_inflate_dynamic_table_size(**session);
  Environment* env = session->env();
  info.GetReturnValue().Set(Integer::New(env->isolate(), size));
}

void Http2Session::GetLocalSettings(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  Environment* env = session->env();
  SESSION_OR_RETURN(session);
  HandleScope scope(env->isolate());
  CHECK_EQ(env->http2settings_constructor_template().IsEmpty(), false);
  Local<Function> constructor =
      env->http2settings_constructor_template()->GetFunction();
  CHECK_EQ(constructor.IsEmpty(), false);
  Local<Object> obj =
      constructor->NewInstance(env->context()).ToLocalChecked();
  new Http2Settings(env, obj, session, true);
  info.GetReturnValue().Set(obj);
}

void Http2Session::SetLocalSettings(
  Local<String> property,
  Local<Value> value,
  const PropertyCallbackInfo<void>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  SESSION_OR_RETURN(session);

  Http2Settings* settings;
  ASSIGN_OR_RETURN_UNWRAP(&settings, value.As<Object>());
  std::vector<nghttp2_settings_entry> entries;
  settings->CollectSettings(&entries);

  nghttp2_submit_settings(**session, NGHTTP2_FLAG_NONE,
                          &entries[0], entries.size());
}

void Http2Session::GetRemoteSettings(
    Local<String> property,
    const PropertyCallbackInfo<Value>& info) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, info.Holder());
  Environment* env = session->env();
  SESSION_OR_RETURN(session);

  HandleScope scope(env->isolate());
  CHECK_EQ(env->http2settings_constructor_template().IsEmpty(), false);
  Local<Function> constructor =
      env->http2settings_constructor_template()->GetFunction();
  CHECK_EQ(constructor.IsEmpty(), false);
  Local<Object> obj =
      constructor->NewInstance(env->context()).ToLocalChecked();
  new Http2Settings(env, obj, session, false);
  info.GetReturnValue().Set(obj);
}


void Http2Session::Destroy(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  SESSION_OR_RETURN(session);
  nghttp2_session_del(session->session_);
  session->session_ = nullptr;
  EMIT0(session->env(), session, "destroy");
}


void Http2Session::Terminate(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  SESSION_OR_RETURN(session);

  uint32_t error_code = args[0]->Uint32Value();
  uint32_t last_proc = nghttp2_session_get_last_proc_stream_id(**session);

  int rv = last_proc > 0 ?
    nghttp2_session_terminate_session2(**session, last_proc, error_code) :
    nghttp2_session_terminate_session(**session, error_code);

  if (rv == 0) {
    rv = nghttp2_session_send(**session);
  }

  args.GetReturnValue().Set(rv);
}

void Http2Session::GracefulTerminate(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  SESSION_OR_RETURN(session);

  int rv = nghttp2_submit_shutdown_notice(**session);
  if (rv == 0) {
    rv = nghttp2_session_send(**session);
  }

  args.GetReturnValue().Set(rv);
}

/**
 * Arguments
 *  stream {integer}
 *  parent (integer)
 *  weight {integer}
 *  exclusive {boolean}
 **/
void Http2Session::CreateIdleStream(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  SESSION_OR_RETURN(session);
  Http2Priority priority(args[1]->Int32Value(),
                         args[2]->Int32Value(),
                         args[3]->BooleanValue());
  args.GetReturnValue().Set(
      nghttp2_session_create_idle_stream(**session,
                                         GetStreamID(args[0]),
                                         *priority));
}

void Http2Session::ReceiveData(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  if (!**session)
    return;

  Environment* env = Environment::GetCurrent(args);
  THROW_AND_RETURN_UNLESS_BUFFER(env, args[0]);
  SPREAD_BUFFER_ARG(args[0], ts_obj);

  uint8_t* data = reinterpret_cast<uint8_t*>(ts_obj_data);
  ssize_t readlen = nghttp2_session_mem_recv(**session, data, ts_obj_length);
  args.GetReturnValue().Set(Integer::NewFromUnsigned(env->isolate(), readlen));
  if (!session->WantReadOrWrite())
    EMIT0(env, session, "canClose");
}


void Http2Session::SendData(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  Environment* env = Environment::GetCurrent(args);
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  if (!**session)
    return;
  args.GetReturnValue().Set(nghttp2_session_send(**session));
  if (!session->WantReadOrWrite())
    EMIT0(env, session, "canClose");
}

void Http2Session::GetStream(const FunctionCallbackInfo<Value>& args) {
  Http2Session* session;
  Environment* env = Environment::GetCurrent(args);
  ASSIGN_OR_RETURN_UNWRAP(&session, args.Holder());
  SESSION_OR_RETURN(session);
  Http2Stream* stream =
      static_cast<Http2Stream*>(
          nghttp2_session_get_stream_user_data(**session,
                                               args[0]->Int32Value()));
  if (stream != nullptr) {
    HandleScope scope(env->isolate());
    args.GetReturnValue().Set(stream->object());
  }
}


void HttpErrorString(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  args.GetReturnValue().Set(
      OneByteString(env->isolate(), nghttp2_strerror(args[0]->Uint32Value())));
}


void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  HandleScope scope(isolate);

  // Method to fetch the nghttp2 string description of an nghttp2 error code
  env->SetMethod(target, "nghttp2ErrorString", HttpErrorString);

  Local<String> http2DataProviderClassName =
     FIXED_ONE_BYTE_STRING(isolate, "Http2DataProvider");
  Local<String> http2HeaderClassName =
    FIXED_ONE_BYTE_STRING(isolate, "Http2Header");
  Local<String> http2SessionClassName =
    FIXED_ONE_BYTE_STRING(isolate, "Http2Session");
  Local<String> http2StreamClassName =
    FIXED_ONE_BYTE_STRING(isolate, "Http2Stream");
  Local<String> http2SettingsClassName =
    FIXED_ONE_BYTE_STRING(isolate, "Http2Settings");

  // Persistent FunctionTemplate for Http2Stream. Instances of this
  // class are only intended to be created by Http2Session::create_stream
  // so the constructor is not exposed via the binding.
  Local<FunctionTemplate> stream_constructor_template =
    Local<FunctionTemplate>(FunctionTemplate::New(isolate));
  stream_constructor_template->SetClassName(http2StreamClassName);
  Local<ObjectTemplate> stream_template =
      stream_constructor_template->InstanceTemplate();
  stream_template->SetInternalFieldCount(1);
  stream_template->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "uid"),
      Http2Stream::GetUid,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  stream_template->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "id"),
      Http2Stream::GetID,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  stream_template->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "state"),
      Http2Stream::GetState,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  stream_template->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "weight"),
      Http2Stream::GetWeight,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  stream_template->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "sumDependencyWeight"),
      Http2Stream::GetSumDependencyWeight,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  stream_template->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "localClose"),
      Http2Stream::GetStreamLocalClose,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  stream_template->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "remoteClose"),
      Http2Stream::GetStreamRemoteClose,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  stream_template->SetAccessor(
    FIXED_ONE_BYTE_STRING(isolate, "localWindowSize"),
    Http2Stream::GetLocalWindowSize,
    Http2Stream::SetLocalWindowSize,
    Local<Value>(),
    v8::DEFAULT,
    v8::DontDelete);
  env->SetProtoMethod(stream_constructor_template,
                      "changeStreamPriority",
                      Http2Stream::ChangeStreamPriority);
  env->SetProtoMethod(stream_constructor_template,
                      "respond",
                      Http2Stream::Respond);
  env->SetProtoMethod(stream_constructor_template,
                      "resumeData",
                      Http2Stream::ResumeData);
  env->SetProtoMethod(stream_constructor_template,
                      "sendContinue",
                      Http2Stream::SendContinue);
  env->SetProtoMethod(stream_constructor_template,
                      "sendTrailers",
                      Http2Stream::SendTrailers);
  env->SetProtoMethod(stream_constructor_template,
                      "sendDataFrame",
                      Http2Stream::SendDataFrame);
  env->SetProtoMethod(stream_constructor_template,
                      "sendPriority",
                      Http2Stream::SendPriority);
  env->SetProtoMethod(stream_constructor_template,
                      "sendRstStream",
                      Http2Stream::SendRstStream);
  env->SetProtoMethod(stream_constructor_template,
                      "sendPushPromise",
                      Http2Stream::SendPushPromise);
  env->set_http2stream_constructor_template(stream_constructor_template);

  // Http2Settings Template
  Local<FunctionTemplate> settings =
      env->NewFunctionTemplate(Http2Settings::New);
  settings->SetClassName(http2SettingsClassName);
  Local<ObjectTemplate> settings_object = settings->InstanceTemplate();
  settings_object->SetInternalFieldCount(1);
  settings_object->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "headerTableSize"),
      Http2Settings::GetHeaderTableSize,
      Http2Settings::SetHeaderTableSize,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  settings_object->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "enablePush"),
      Http2Settings::GetEnablePush,
      Http2Settings::SetEnablePush,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  settings_object->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "maxConcurrentStreams"),
      Http2Settings::GetMaxConcurrentStreams,
      Http2Settings::SetMaxConcurrentStreams,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  settings_object->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "initialWindowSize"),
      Http2Settings::GetInitialWindowSize,
      Http2Settings::SetInitialWindowSize,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  settings_object->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "maxFrameSize"),
      Http2Settings::GetMaxFrameSize,
      Http2Settings::SetMaxFrameSize,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  settings_object->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "maxHeaderListSize"),
      Http2Settings::GetMaxHeaderListSize,
      Http2Settings::SetMaxHeaderListSize,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  env->SetProtoMethod(settings, "setDefaults", Http2Settings::Defaults);
  env->SetProtoMethod(settings, "reset", Http2Settings::Reset);
  env->SetProtoMethod(settings, "pack", Http2Settings::Pack);
  env->set_http2settings_constructor_template(settings);
  target->Set(context,
              http2SettingsClassName,
              settings->GetFunction()).FromJust();

  // Http2DataProvider Template
  Local<FunctionTemplate> provider =
      env->NewFunctionTemplate(Http2DataProvider::New);
  provider->InstanceTemplate()->SetInternalFieldCount(1);
  provider->SetClassName(http2DataProviderClassName);
  target->Set(context,
              http2DataProviderClassName,
              provider->GetFunction()).FromJust();

  // Http2Header Template
  Local<FunctionTemplate> header =
      env->NewFunctionTemplate(Http2Header::New);
  header->InstanceTemplate()->SetInternalFieldCount(1);
  header->SetClassName(http2HeaderClassName);
  header->InstanceTemplate()->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "name"),
      Http2Header::GetName,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  header->InstanceTemplate()->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "value"),
      Http2Header::GetValue,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  header->InstanceTemplate()->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "flags"),
      Http2Header::GetFlags,
      Http2Header::SetFlags,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  target->Set(context,
              http2HeaderClassName,
              header->GetFunction()).FromJust();

  // Http2Session Template
  Local<FunctionTemplate> t =
      env->NewFunctionTemplate(Http2Session::New);
  t->SetClassName(http2SessionClassName);
  Local<ObjectTemplate> instance = t->InstanceTemplate();
  instance->SetInternalFieldCount(1);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "uid"),
      Http2Session::GetUid,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "wantWrite"),
      Http2Session::GetWantWrite,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "wantRead"),
      Http2Session::GetWantRead,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "root"),
      Http2Session::GetRootStream,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "type"),
      Http2Session::GetType,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "nextStreamID"),
      Http2Session::GetNextStreamID,
      Http2Session::SetNextStreamID,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "effectiveLocalWindowSize"),
      Http2Session::GetEffectiveLocalWindowSize,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "effectiveRecvDataLength"),
      Http2Session::GetEffectiveRecvDataLength,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "lastProcStreamID"),
      Http2Session::GetLastProcStreamID,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "outboundQueueSize"),
      Http2Session::GetOutboundQueueSize,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "remoteWindowSize"),
      Http2Session::GetRemoteWindowSize,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "deflateDynamicTableSize"),
      Http2Session::GetDeflateDynamicTableSize,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "inflateDynamicTableSize"),
      Http2Session::GetInflateDynamicTableSize,
      nullptr,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
      FIXED_ONE_BYTE_STRING(isolate, "localWindowSize"),
      Http2Session::GetLocalWindowSize,
      Http2Session::SetLocalWindowSize,
      Local<Value>(),
      v8::DEFAULT,
      v8::DontDelete);
  instance->SetAccessor(
    FIXED_ONE_BYTE_STRING(isolate, "localSettings"),
    Http2Session::GetLocalSettings,
    Http2Session::SetLocalSettings,
    Local<Value>(),
    v8::DEFAULT,
    v8::DontDelete);
  instance->SetAccessor(
    FIXED_ONE_BYTE_STRING(isolate, "remoteSettings"),
    Http2Session::GetRemoteSettings,
    nullptr,
    Local<Value>(),
    v8::DEFAULT,
    v8::DontDelete);

  env->SetProtoMethod(t, "gracefulTerminate", Http2Session::GracefulTerminate);
  env->SetProtoMethod(t, "destroy", Http2Session::Destroy);
  env->SetProtoMethod(t, "terminate", Http2Session::Terminate);
  env->SetProtoMethod(t, "createIdleStream", Http2Session::CreateIdleStream);
  env->SetProtoMethod(t, "sendData", Http2Session::SendData);
  env->SetProtoMethod(t, "receiveData", Http2Session::ReceiveData);
  env->SetProtoMethod(t, "getStream", Http2Session::GetStream);


  target->Set(context,
              http2SessionClassName,
              t->GetFunction()).FromJust();

  Local<Object> constants = Object::New(isolate);
  NODE_DEFINE_CONSTANT(constants, SESSION_TYPE_SERVER);
  NODE_DEFINE_CONSTANT(constants, SESSION_TYPE_CLIENT);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_STATE_IDLE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_STATE_OPEN);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_STATE_RESERVED_LOCAL);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_STATE_RESERVED_REMOTE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_STATE_HALF_CLOSED_LOCAL);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_STATE_HALF_CLOSED_REMOTE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_STATE_CLOSED);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_HCAT_REQUEST);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_HCAT_RESPONSE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_HCAT_PUSH_RESPONSE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_HCAT_HEADERS);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_NO_ERROR);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_PROTOCOL_ERROR);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_INTERNAL_ERROR);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FLOW_CONTROL_ERROR);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_SETTINGS_TIMEOUT);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_STREAM_CLOSED);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_FRAME_SIZE_ERROR);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_REFUSED_STREAM);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_CANCEL);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_COMPRESSION_ERROR);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_CONNECT_ERROR);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_ENHANCE_YOUR_CALM);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_INADEQUATE_SECURITY);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_HTTP_1_1_REQUIRED);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_NV_FLAG_NONE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_NV_FLAG_NO_INDEX);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_NV_FLAG_NO_COPY_NAME);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_NV_FLAG_NO_COPY_VALUE);
  NODE_DEFINE_CONSTANT(constants, NGHTTP2_ERR_DEFERRED);

  NODE_DEFINE_STRING_CONSTANT(constants,
                              "HTTP2_HEADER_STATUS",
                              HTTP2_HEADER_STATUS);
  NODE_DEFINE_STRING_CONSTANT(constants,
                              "HTTP2_HEADER_METHOD",
                              HTTP2_HEADER_METHOD);
  NODE_DEFINE_STRING_CONSTANT(constants,
                              "HTTP2_HEADER_AUTHORITY",
                              HTTP2_HEADER_AUTHORITY);
  NODE_DEFINE_STRING_CONSTANT(constants,
                              "HTTP2_HEADER_SCHEME",
                              HTTP2_HEADER_SCHEME);
  NODE_DEFINE_STRING_CONSTANT(constants,
                              "HTTP2_HEADER_PATH",
                              HTTP2_HEADER_PATH);

#define V(name, _) NODE_DEFINE_CONSTANT(constants, HTTP_STATUS_##name);
HTTP_STATUS_CODES(V)
#undef V

#define V(name) NODE_DEFINE_CONSTANT(constants, FLAG_##name);
DATA_FLAGS(V)
#undef V

  target->Set(context,
              FIXED_ONE_BYTE_STRING(isolate, "constants"),
              constants).FromJust();
}
}  // namespace http2
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(http2, node::http2::Initialize)
