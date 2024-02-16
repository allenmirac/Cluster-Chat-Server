#include "chatservice.hpp"
#include "public.hpp"
#include "user.hpp"
#include "usermodel.hpp"
#include <muduo/base/Logging.h>

ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

ChatService::ChatService()
{
    msgHandlerMap_.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    msgHandlerMap_.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
    msgHandlerMap_.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
}

ChatService::~ChatService()
{
    msgHandlerMap_.clear();
}

MsgHandler ChatService::getHandler(int msgid)
{
    auto it = msgHandlerMap_.find(msgid);
    if (it == msgHandlerMap_.end())
    {
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp)
        {
            LOG_ERROR << "Msgid is " << msgid << " can not find handler!";
        };
    }
    else
    {
        return msgHandlerMap_[msgid];
    }
}

void ChatService::reset()
{
    userModel_.resetState();
}

// 处理登陆业务
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int id = js["id"];
    string pwd = js["password"];

    User userQuery;
    userQuery = userModel_.query(id);
    if (userQuery.getId() == id && userQuery.getPassword() == pwd)
    {
        if (userQuery.getState() == "online")
        {
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errormsg"] = "该账号已经登陆，请重新输入账号";
            conn->send(response.dump());
        }
        else
        {
            // 连接成功
            {
                // 锁的粒度尽可能小
                lock_guard<mutex> lock(mutex_);
                userConnMap_.insert({id, conn});
                LOG_INFO << "ChatService::login, 用户 ["<< userQuery.getName() <<"] 登陆成功";
            }
            userQuery.setState("online");
            userModel_.updateState(userQuery);
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["id"] = userQuery.getId();
            response["name"] = userQuery.getName();

            //
            vector<string> v = offlineMsgModel_.query(id);
            if (!v.empty())
            {
                response["offlinemessage"] = v;
                // LOG_INFO << "v:" << v[0];
                offlineMsgModel_.remove(id);
            }
            conn->send(response.dump());
        }
    }
    else
    {
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errormsg"] = "账号或密码输入错误";
        conn->send(response.dump());
    }
    // LOG_INFO << "login 回调处理";
}

void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["name"];
    string password = js["password"];

    User user;
    user.setName(name);
    user.setPassword(password);
    bool isInsert = userModel_.insert(user);
    if (isInsert)
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        conn->send(response.dump());
    }
    else
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn->send(response.dump());
    }
}

void ChatService::clientQuitEcption(const TcpConnectionPtr &conn)
{
    User user;
    {

        lock_guard<mutex> lock(mutex_);
        for (auto it = userConnMap_.begin(); it != userConnMap_.end(); it++)
        {
            if (it->second == conn)
            {
                user.setId(it->first);
                userConnMap_.erase(it);
                break;
            }
        }
    }
    if (user.getId() != -1)
    {
        user.setState("offline");
        userModel_.updateState(user);
        LOG_ERROR << "ChatService::clientQuitEcption, 用户下线";
    }
    else
    {
        LOG_ERROR << "ChatService::clientQuitEcption, 无客户";
    }
}

void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int toId = js["to"];
    {
        lock_guard<mutex> lock(mutex_);
        auto it = userConnMap_.find(toId);
        if (it != userConnMap_.end())
        {
            // 用户在线
            LOG_INFO << "ChatService::oneChat, 用户在线";
            it->second->send(js.dump());
            return;
        }
    }
    // 用户不在线
    offlineMsgModel_.insert(toId, js.dump());
    LOG_INFO << "ChatService::oneChat, 用户不在线, 保存离线数据";
}