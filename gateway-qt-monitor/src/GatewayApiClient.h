#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QNetworkAccessManager>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <functional>

class GatewayApiClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString baseUrl READ baseUrl WRITE setBaseUrl NOTIFY baseUrlChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString historyError READ historyError NOTIFY historyErrorChanged)
    Q_PROPERTY(QString systemHistoryError READ systemHistoryError NOTIFY systemHistoryErrorChanged)

    Q_PROPERTY(QVariantMap status READ status NOTIFY dataUpdated)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY dataUpdated)
    Q_PROPERTY(QVariantList recent READ recent NOTIFY dataUpdated)
    Q_PROPERTY(QVariantList commandRecent READ commandRecent NOTIFY dataUpdated)
    Q_PROPERTY(QVariantList otaTasks READ otaTasks NOTIFY otaTasksUpdated)
    Q_PROPERTY(QVariantList otaTaskEvents READ otaTaskEvents NOTIFY otaTaskEventsUpdated)
    Q_PROPERTY(QVariantMap ackDebug READ ackDebug NOTIFY dataUpdated)
    Q_PROPERTY(QVariantList historyEvents READ historyEvents NOTIFY historyUpdated)
    Q_PROPERTY(QVariantList systemEvents READ systemEvents NOTIFY historyUpdated)

public:
    explicit GatewayApiClient(QObject* parent = nullptr);

    QString baseUrl() const { return base_url_; }
    void setBaseUrl(const QString& url);

    bool loading() const { return loading_; }
    QString lastError() const { return last_error_; }
    QString historyError() const { return history_error_; }
    QString systemHistoryError() const { return system_history_error_; }

    QVariantMap status() const { return status_; }
    QVariantList devices() const { return devices_; }
    QVariantList recent() const { return recent_; }
    QVariantList commandRecent() const { return command_recent_; }
    QVariantList otaTasks() const { return ota_tasks_; }
    QVariantList otaTaskEvents() const { return ota_task_events_; }
    QVariantMap ackDebug() const { return ack_debug_; }
    QVariantList historyEvents() const { return history_events_; }
    QVariantList systemEvents() const { return system_events_; }

    Q_INVOKABLE void refreshNow();
    Q_INVOKABLE void fetchHistory(int deviceId, int limit, int offset);
    Q_INVOKABLE void fetchSystemHistory(int limit, int offset);
    Q_INVOKABLE void refreshOtaTasks();
    Q_INVOKABLE void fetchOtaTaskEvents(const QString& taskUuid);
    Q_INVOKABLE void postJson(const QString& path, const QVariantMap& jsonBody, int timeoutMs = 3000);
    Q_INVOKABLE void startPolling(int intervalMs = 1000);
    Q_INVOKABLE void stopPolling();

signals:
    void baseUrlChanged();
    void loadingChanged();
    void lastErrorChanged();
    void historyErrorChanged();
    void systemHistoryErrorChanged();
    void dataUpdated();
    void historyUpdated();
    void otaTasksUpdated();
    void otaTaskEventsUpdated();
    void jsonResponseReceived(const QString& path, int httpStatus, const QVariantMap& response, const QString& error);

private:
    void setLoading(bool v);
    void setLastError(const QString& err);
    void setHistoryError(const QString& err);
    void setSystemHistoryError(const QString& err);
    void updateRequestError(const QString& path, const QString& err);
    QString errorSummary() const;
    void fetchPath(const QString& path,
                   int timeoutMs,
                   bool allowConcurrent,
                   const std::function<void(const QByteArray&, int, const QString&)>& onDone);
    void requestPath(const QString& path,
                     const QByteArray& method,
                     const QByteArray& body,
                     int timeoutMs,
                     bool allowConcurrent,
                     const std::function<void(const QByteArray&, int, const QString&)>& onOk);

    QString base_url_ = "http://127.0.0.1:9010";
    bool loading_ = false;
    int inflight_ = 0;
    QString last_error_;
    QString history_error_;
    QString system_history_error_;
    QSet<QString> active_requests_;
    QVariantMap request_errors_;

    QVariantMap status_;
    QVariantList devices_;
    QVariantList recent_;
    QVariantMap ack_debug_;
    QVariantList history_events_;
    QVariantList system_events_;
    QVariantList command_recent_;
    QVariantList ota_tasks_;
    QVariantList ota_task_events_;

    QNetworkAccessManager net_;
    QTimer poll_timer_;
};
