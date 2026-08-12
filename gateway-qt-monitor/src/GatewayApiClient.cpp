#include "GatewayApiClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>
#include <QTimer>

#include <functional>

namespace {

constexpr int kPollingTimeoutMs = 1500;
constexpr int kManualTimeoutMs = 3000;

}

GatewayApiClient::GatewayApiClient(QObject* parent) : QObject(parent) {
    poll_timer_.setSingleShot(false);
    connect(&poll_timer_, &QTimer::timeout, this, &GatewayApiClient::refreshNow);
}

void GatewayApiClient::setBaseUrl(const QString& url) {
    QString normalized = url.trimmed();
    while (normalized.endsWith('/')) normalized.chop(1);
    if (normalized.isEmpty()) return;
    if (normalized == base_url_) return;
    base_url_ = normalized;
    emit baseUrlChanged();
}

void GatewayApiClient::setLoading(bool v) {
    if (loading_ == v) return;
    loading_ = v;
    emit loadingChanged();
}

void GatewayApiClient::setLastError(const QString& err) {
    if (last_error_ == err) return;
    last_error_ = err;
    emit lastErrorChanged();
}

void GatewayApiClient::setHistoryError(const QString& err) {
    if (history_error_ == err) return;
    history_error_ = err;
    emit historyErrorChanged();
}

void GatewayApiClient::setSystemHistoryError(const QString& err) {
    if (system_history_error_ == err) return;
    system_history_error_ = err;
    emit systemHistoryErrorChanged();
}

QString GatewayApiClient::errorSummary() const {
    QStringList lines;
    const auto keys = request_errors_.keys();
    for (const auto& key : keys) {
        const QString value = request_errors_.value(key).toString().trimmed();
        if (!value.isEmpty()) {
            lines.push_back(key + ": " + value);
        }
    }
    return lines.join(" | ");
}

void GatewayApiClient::updateRequestError(const QString& path, const QString& err) {
    if (err.trimmed().isEmpty()) {
        request_errors_.remove(path);
    } else {
        request_errors_.insert(path, err.trimmed());
    }
    setLastError(errorSummary());
}

void GatewayApiClient::requestPath(const QString& path,
                                   const QByteArray& method,
                                   const QByteArray& body,
                                   int timeoutMs,
                                   bool allowConcurrent,
                                   const std::function<void(const QByteArray&, int, const QString&)>& onOk) {
    if (!allowConcurrent && active_requests_.contains(path)) {
        return;
    }

    QNetworkRequest req(QUrl(base_url_ + path));
    req.setHeader(QNetworkRequest::UserAgentHeader, "gateway-qt-monitor/1.0");

    active_requests_.insert(path);
    inflight_++;
    setLoading(true);

    QNetworkReply* reply = nullptr;
    if (method == "POST") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
        reply = net_.post(req, body);
    } else {
        reply = net_.get(req);
    }

    QTimer* timeoutTimer = nullptr;
    if (timeoutMs > 0) {
        timeoutTimer = new QTimer(reply);
        timeoutTimer->setSingleShot(true);
        connect(timeoutTimer, &QTimer::timeout, reply, [reply]() {
            reply->abort();
        });
        timeoutTimer->start(timeoutMs);
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, onOk, timeoutTimer, path]() {
        if (timeoutTimer != nullptr) {
            timeoutTimer->stop();
        }
        const QByteArray body = reply->readAll();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            const QString err = (reply->error() == QNetworkReply::OperationCanceledError)
                ? QStringLiteral("request timeout")
                : reply->errorString();
            onOk(body, httpStatus, err);
        } else {
            onOk(body, httpStatus, QString());
        }
        reply->deleteLater();
        active_requests_.remove(path);

        inflight_--;
        if (inflight_ <= 0) {
            inflight_ = 0;
            setLoading(false);
        }
    });
}

void GatewayApiClient::fetchPath(const QString& path,
                                 int timeoutMs,
                                 bool allowConcurrent,
                                 const std::function<void(const QByteArray&, int, const QString&)>& onDone) {
    requestPath(path, "GET", QByteArray(), timeoutMs, allowConcurrent, onDone);
}

void GatewayApiClient::refreshNow() {
    fetchPath("/api/status", kPollingTimeoutMs, false, [this](const QByteArray& body, int, const QString& err) {
        if (!err.isEmpty()) {
            status_.clear();
            updateRequestError("/api/status", err);
            emit dataUpdated();
            return;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(body, &parse_error);
        if (!doc.isObject()) {
            status_.clear();
            updateRequestError("/api/status",
                               parse_error.error == QJsonParseError::NoError
                                   ? QStringLiteral("response is not JSON object")
                                   : parse_error.errorString());
            emit dataUpdated();
            return;
        }
        updateRequestError("/api/status", QString());
        status_ = doc.object().toVariantMap();
        emit dataUpdated();
    });

    fetchPath("/api/devices", kPollingTimeoutMs, false, [this](const QByteArray& body, int, const QString& err) {
        if (!err.isEmpty()) {
            devices_.clear();
            updateRequestError("/api/devices", err);
            emit dataUpdated();
            return;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(body, &parse_error);
        if (!doc.isArray()) {
            devices_.clear();
            updateRequestError("/api/devices",
                               parse_error.error == QJsonParseError::NoError
                                   ? QStringLiteral("response is not JSON array")
                                   : parse_error.errorString());
            emit dataUpdated();
            return;
        }
        updateRequestError("/api/devices", QString());
        devices_ = doc.array().toVariantList();
        emit dataUpdated();
    });

    fetchPath("/api/recent", kPollingTimeoutMs, false, [this](const QByteArray& body, int, const QString& err) {
        if (!err.isEmpty()) {
            recent_.clear();
            updateRequestError("/api/recent", err);
            emit dataUpdated();
            return;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(body, &parse_error);
        if (!doc.isArray()) {
            recent_.clear();
            updateRequestError("/api/recent",
                               parse_error.error == QJsonParseError::NoError
                                   ? QStringLiteral("response is not JSON array")
                                   : parse_error.errorString());
            emit dataUpdated();
            return;
        }
        updateRequestError("/api/recent", QString());
        recent_ = doc.array().toVariantList();
        emit dataUpdated();
    });

    fetchPath("/api/commands/recent", kPollingTimeoutMs, false, [this](const QByteArray& body, int, const QString& err) {
        if (!err.isEmpty()) {
            command_recent_.clear();
            updateRequestError("/api/commands/recent", err);
            emit dataUpdated();
            return;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(body, &parse_error);
        if (!doc.isObject()) {
            command_recent_.clear();
            updateRequestError("/api/commands/recent",
                               parse_error.error == QJsonParseError::NoError
                                   ? QStringLiteral("response is not JSON object")
                                   : parse_error.errorString());
            emit dataUpdated();
            return;
        }
        const auto root = doc.object();
        const auto commands = root.value("commands");
        if (!commands.isArray()) {
            command_recent_.clear();
            updateRequestError("/api/commands/recent", QStringLiteral("commands is not JSON array"));
            emit dataUpdated();
            return;
        }
        updateRequestError("/api/commands/recent", QString());
        command_recent_ = commands.toArray().toVariantList();
        emit dataUpdated();
    });

    refreshOtaTasks();

    fetchPath("/api/debug_ack", kPollingTimeoutMs, false, [this](const QByteArray& body, int, const QString& err) {
        if (!err.isEmpty()) {
            ack_debug_.clear();
            updateRequestError("/api/debug_ack", err);
            emit dataUpdated();
            return;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(body, &parse_error);
        if (!doc.isObject()) {
            ack_debug_.clear();
            updateRequestError("/api/debug_ack",
                               parse_error.error == QJsonParseError::NoError
                                   ? QStringLiteral("response is not JSON object")
                                   : parse_error.errorString());
            emit dataUpdated();
            return;
        }
        updateRequestError("/api/debug_ack", QString());
        ack_debug_ = doc.object().toVariantMap();
        emit dataUpdated();
    });
}

void GatewayApiClient::postJson(const QString& path, const QVariantMap& jsonBody, int timeoutMs) {
    const QByteArray payload = QJsonDocument::fromVariant(jsonBody).toJson(QJsonDocument::Compact);
    requestPath(path, "POST", payload, timeoutMs > 0 ? timeoutMs : kManualTimeoutMs, true,
                [this, path](const QByteArray& body, int httpStatus, const QString& err) {
                    QVariantMap response;
                    QString parseError;
                    const auto doc = QJsonDocument::fromJson(body);
                    if (doc.isObject()) {
                        response = doc.object().toVariantMap();
                    } else if (!body.isEmpty()) {
                        parseError = "response is not JSON object";
                    }
                    emit jsonResponseReceived(path, httpStatus, response, err.isEmpty() ? parseError : err);
                });
}

void GatewayApiClient::refreshOtaTasks() {
    fetchPath("/api/ota/tasks", kPollingTimeoutMs, false, [this](const QByteArray& body, int, const QString& err) {
        if (!err.isEmpty()) {
            ota_tasks_.clear();
            updateRequestError("/api/ota/tasks", err);
            emit otaTasksUpdated();
            return;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(body, &parse_error);
        if (!doc.isArray()) {
            ota_tasks_.clear();
            updateRequestError("/api/ota/tasks",
                               parse_error.error == QJsonParseError::NoError
                                   ? QStringLiteral("response is not JSON array")
                                   : parse_error.errorString());
            emit otaTasksUpdated();
            return;
        }
        updateRequestError("/api/ota/tasks", QString());
        ota_tasks_ = doc.array().toVariantList();
        emit otaTasksUpdated();
    });
}

void GatewayApiClient::fetchOtaTaskEvents(const QString& taskUuid) {
    const QString path = QString("/api/ota/tasks/%1/events").arg(taskUuid);
    fetchPath(path, kPollingTimeoutMs, false, [this, path](const QByteArray& body, int, const QString& err) {
        if (!err.isEmpty()) {
            ota_task_events_.clear();
            updateRequestError(path, err);
            emit otaTaskEventsUpdated();
            return;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(body, &parse_error);
        if (!doc.isArray()) {
            ota_task_events_.clear();
            updateRequestError(path,
                               parse_error.error == QJsonParseError::NoError
                                   ? QStringLiteral("response is not JSON array")
                                   : parse_error.errorString());
            emit otaTaskEventsUpdated();
            return;
        }
        updateRequestError(path, QString());
        ota_task_events_ = doc.array().toVariantList();
        emit otaTaskEventsUpdated();
    });
}

void GatewayApiClient::fetchHistory(int deviceId, int limit, int offset) {
    const QString path = QString("/api/history?device_id=%1&limit=%2&offset=%3")
        .arg(deviceId).arg(limit).arg(offset);
    fetchPath(path, kManualTimeoutMs, true, [this, path](const QByteArray& body, int, const QString& err_text) {
        if (!err_text.isEmpty()) {
            setHistoryError("传感器历史请求失败: " + err_text);
            history_events_.clear();
            emit historyUpdated();
            return;
        }
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError) {
            setHistoryError("传感器历史JSON解析失败: " + err.errorString());
            history_events_.clear();
        } else if (doc.isObject() && doc.object().contains("error")) {
            setHistoryError("传感器历史: " + doc.object()["error"].toString());
            history_events_.clear();
        } else if (doc.isArray()) {
            history_events_ = doc.array().toVariantList();
            setHistoryError(QString());
        } else {
            setHistoryError("传感器历史返回格式异常");
            history_events_.clear();
        }
        updateRequestError(path, QString());
        emit historyUpdated();
    });
}

void GatewayApiClient::fetchSystemHistory(int limit, int offset) {
    const QString path = QString("/api/history/system?limit=%1&offset=%2")
        .arg(limit).arg(offset);
    fetchPath(path, kManualTimeoutMs, true, [this, path](const QByteArray& body, int, const QString& err_text) {
        if (!err_text.isEmpty()) {
            setSystemHistoryError("系统事件请求失败: " + err_text);
            system_events_.clear();
            emit historyUpdated();
            return;
        }
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError) {
            setSystemHistoryError("系统事件JSON解析失败: " + err.errorString());
            system_events_.clear();
        } else if (doc.isObject() && doc.object().contains("error")) {
            setSystemHistoryError("系统事件: " + doc.object()["error"].toString());
            system_events_.clear();
        } else if (doc.isArray()) {
            system_events_ = doc.array().toVariantList();
            setSystemHistoryError(QString());
        } else {
            setSystemHistoryError("系统事件返回格式异常");
            system_events_.clear();
        }
        updateRequestError(path, QString());
        emit historyUpdated();
    });
}

void GatewayApiClient::startPolling(int intervalMs) {
    if (intervalMs < 300) intervalMs = 300;
    poll_timer_.start(intervalMs);
    refreshNow();
}

void GatewayApiClient::stopPolling() {
    poll_timer_.stop();
}
