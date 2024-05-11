#include <iostream>
#include "PluginApi.hpp"
#include "zenohc.hxx"
#include "Utils.hpp"

using namespace std;

namespace PluggableTransport {

class SubscriberImpl;

static zenohc::Session inst()
{
    zenohc::Config config;
    return zenohc::expect<zenohc::Session>(zenohc::open(std::move(config)));
}

struct SessionImpl : public SessionApi {
    string start_doc;
    zenohc::Session session;

    SessionImpl(const string& start_doc) : start_doc(start_doc), session(inst())
    {
    }
};

struct PublisherImpl : public PublisherApi {
    shared_ptr<SessionImpl> session;
    string sending_topic;
    z_owned_publisher_t handle;
    
    PublisherImpl(shared_ptr<SessionApi> session_base, const string& sending_topic) : sending_topic(sending_topic)
    {
        session = dynamic_pointer_cast<SessionImpl>(session_base);
        handle = z_declare_publisher(session->session.loan(), z_keyexpr(sending_topic.c_str()), nullptr);
        if (!z_check(handle)) throw std::runtime_error("Cannot declare publisher");
    }

    ~PublisherImpl()
    {
        z_undeclare_publisher(&handle);
    }

    void operator()(const Message& message)
    {
        z_publisher_put_options_t options = z_publisher_put_options_default();
        z_owned_bytes_map_t map = z_bytes_map_new();
        options.attachment = z_bytes_map_as_attachment(&map);
        z_bytes_map_insert_by_alias(&map, z_bytes_new("attributes"), z_bytes_t{.len=message.attributes.size(), .start=(const uint8_t*)message.attributes.data()});
        if (z_publisher_put(z_loan(handle), (const uint8_t*)message.payload.data(), message.payload.size(), &options)) {
            z_drop(z_move(map));
            throw std::runtime_error("Cannot publish");
        }
        z_drop(z_move(map));
    }
};

struct SubInfo {
    string  sending_topic;
    Message message;

    SubInfo(const zenohc::Sample& sample)
    {
        sending_topic = sample.get_keyexpr().as_string_view();
        message.payload = sample.get_payload().as_string_view();
        message.attributes = sample.get_attachment().get("attributes").as_string_view();
    } 
};

struct SubscriberImpl : public SubscriberApi {
    shared_ptr<SessionImpl> session;
    unique_ptr<zenohc::Subscriber> handle;
    // zenohc::KeyExprView listening_topic;
    string listening_topic;
    Fifo<SubInfo> fifo;
    unique_ptr<ThreadPool> pool;
    SubscriberServerCallback callback;

    void handler(const zenohc::Sample& sample)
    {
        fifo.push(make_shared<SubInfo>(sample));
    }

    void worker()
    {
        while (true) {
            auto ptr = fifo.pull();
            if (ptr == nullptr) return;
            callback(ptr->sending_topic, listening_topic, ptr->message);
        }
    }

    SubscriberImpl(shared_ptr<SessionApi> session_base, const std::string& listening_topic, SubscriberServerCallback callback, size_t thread_count)
        : listening_topic(listening_topic), callback(callback)
    {
        session = dynamic_pointer_cast<SessionImpl>(session_base);
        handle = std::make_unique<zenohc::Subscriber>(
            zenohc::expect<zenohc::Subscriber>(
                session->session.declare_subscriber(
                    listening_topic,
                    [&](const zenohc::Sample& arg) { this->handler(arg); } )));
        pool = make_unique<ThreadPool>([&]() { worker(); }, thread_count);
    }

    ~SubscriberImpl()
    {
        fifo.exit();
    }
};

static std::string keyexpr2string(const z_keyexpr_t& keyexpr)
{
    z_owned_str_t keystr = z_keyexpr_to_string(keyexpr);
    std::string ret(z_loan(keystr));
    z_drop(z_move(keystr));
    return ret;    
}

static string extract(const z_bytes_t& b)
{
    auto ptr = (const char*)b.start;
    return string(ptr, ptr+b.len);
}

static z_bytes_t pack(const string_view& od)
{
    return z_bytes_t{.len = od.size(), .start = (uint8_t*)od.data()};
}

struct RpcClientImpl : public RpcClientApi {
    shared_ptr<SessionImpl> session;
    z_owned_reply_channel_t channel;
    
    RpcClientImpl(shared_ptr<SessionApi> session_base, const string& expr, const Message& message, const std::chrono::seconds& timeout)
    {
        session = dynamic_pointer_cast<SessionImpl>(session_base);
        z_keyexpr_t keyexpr = z_keyexpr(expr.c_str());
        if (!z_check(keyexpr)) throw std::runtime_error("Not a valid key expression");
        channel = zc_reply_fifo_new(16);
        auto opts = z_get_options_default();
        auto attrs = z_bytes_map_new();
        opts.value.payload = pack(message.payload);
        z_bytes_map_insert_by_alias(&attrs, z_bytes_new("attributes"), pack(message.attributes));
        opts.attachment = z_bytes_map_as_attachment(&attrs);
        opts.timeout_ms = chrono::milliseconds(timeout).count();
        z_get(session->session.loan(), keyexpr, "", z_move(channel.send), &opts);
    }

    ~RpcClientImpl()
    {
        z_drop(z_move(channel));
    }

    tuple<string, Message> operator()()
    {
        std::string src;
        string payload, attributes;
        z_owned_reply_t reply = z_reply_null();
        tuple<string, Message> results;

        for (z_call(channel.recv, &reply); z_check(reply); z_call(channel.recv, &reply)) {
            if (z_reply_is_ok(&reply)) {
                z_sample_t sample = z_reply_ok(&reply);
                get<0>(results) = keyexpr2string(sample.keyexpr);
                get<1>(results).payload = extract(sample.payload);
                z_bytes_t attr = z_attachment_get(sample.attachment, z_bytes_new("attributes"));
                get<1>(results).attributes = extract(attr);
                break;
            } else {
                throw std::runtime_error("RPC client timed out.");
            }
        }

        z_drop(z_move(reply));
        return results;
    }
};

struct RpcInfo {
    string  keyexpr;
    Message message;
    z_owned_query_t owned_query;

    RpcInfo(const z_query_t *query)
    {
        keyexpr = keyexpr2string(z_query_keyexpr(query));
        // z_bytes_t pred = z_query_parameters(query);
        z_value_t value = z_query_value(query);
        message.payload = extract(value.payload);

        z_attachment_t attachment = z_query_attachment(query);
        if (!z_check(attachment)) throw std::runtime_error("attachment is missing");
        z_bytes_t avalue = z_attachment_get(attachment, z_bytes_new("attributes"));
        message.attributes = extract(avalue);
        owned_query = z_query_clone(query);
    }

    ~RpcInfo()
    {
        z_query_drop(&owned_query);
    }
};

struct RpcServerImpl : public RpcServerApi {
    shared_ptr<SessionImpl> session;
    z_owned_queryable_t qable;
    Fifo<RpcInfo> fifo;
    unique_ptr<ThreadPool> pool;
    RpcServerCallback callback;

    static void _handler(const z_query_t *query, void *context)
    {
        reinterpret_cast<RpcServerImpl*>(context)->handler(query);
    }

    void handler(const z_query_t *query)
    {
        fifo.push(make_shared<RpcInfo>(query));
    }

    void worker()
    {
        while (true) {
            auto ptr = fifo.pull();
            if (ptr == nullptr) return;
            auto results = callback(ptr->keyexpr, ptr->message);
            if (results) {
                auto& payload = results->payload;
                auto& attributes = results->attributes;
                auto query = z_query_loan(&ptr->owned_query);
                z_query_reply_options_t options = z_query_reply_options_default();
                z_owned_bytes_map_t map = z_bytes_map_new();
                z_bytes_map_insert_by_alias(&map, z_bytes_new("attributes"), pack(attributes));
                options.attachment = z_bytes_map_as_attachment(&map);
                z_query_reply(&query, z_keyexpr(ptr->keyexpr.c_str()), (const uint8_t*)payload.data(), payload.size(), &options);                
            }
            else {
                cout << "no results to send" << endl;
            }
        }
    }

    RpcServerImpl(shared_ptr<SessionApi> session_base, const std::string& keyexpr, RpcServerCallback callback, size_t thread_count) : callback(callback)
    {
        session = dynamic_pointer_cast<SessionImpl>(session_base);
        cout << "registering RPC callback for " << keyexpr << " thread_count=" << thread_count << endl;

        z_owned_closure_query_t closure = z_closure(_handler, NULL, this);
        qable = z_declare_queryable(session->session.loan(), z_keyexpr(keyexpr.c_str()), z_move(closure), NULL);
        if (!z_check(qable)) throw std::runtime_error("Unable to create queryable.");
        pool = make_unique<ThreadPool>([&]() { worker(); }, thread_count);
    }

    ~RpcServerImpl()
    {
        fifo.exit();
        z_undeclare_queryable(z_move(qable));
    }
};

Factories factories = {
    [](const auto start_doc) { return make_shared<SessionImpl>(start_doc); },
    [](auto session_base, auto ...args) { return make_shared<PublisherImpl>(session_base, args...); },
    [](auto session_base, auto ...args) { return make_shared<SubscriberImpl>(session_base, args...); },
    [](auto session_base, auto ...args) { return make_shared<RpcClientImpl>(session_base, args...); },
    [](auto session_base, auto ...args) { return make_shared<RpcServerImpl>(session_base, args...); },
};

}; // PluggableTransport

FACTORY_EXPOSE(PluggableTransport::factories)
