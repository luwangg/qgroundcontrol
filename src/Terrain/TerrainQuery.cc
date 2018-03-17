/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "TerrainQuery.h"

#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

QGC_LOGGING_CATEGORY(TerrainQueryLog, "TerrainQueryLog")

Q_GLOBAL_STATIC(TerrainAtCoordinateBatchManager, _TerrainAtCoordinateBatchManager)

TerrainQuery::TerrainQuery(QObject* parent)
    : QObject(parent)
{

}

void TerrainQuery::_sendQuery(const QString& path, const QUrlQuery& urlQuery)
{
    QUrl url(QStringLiteral("https://api.airmap.com/elevation/v1/ele") + path);
    url.setQuery(urlQuery);

    QNetworkRequest request(url);

    QNetworkProxy tProxy;
    tProxy.setType(QNetworkProxy::DefaultProxy);
    _networkManager.setProxy(tProxy);

    QNetworkReply* networkReply = _networkManager.get(request);
    if (!networkReply) {
        qCDebug(TerrainQueryLog) << "QNetworkManager::Get did not return QNetworkReply";
        _terrainData(false /* success */ , QJsonValue());
        return;
    }

    connect(networkReply, &QNetworkReply::finished, this, &TerrainQuery::_requestFinished);
}

void TerrainQuery::_requestFinished(void)
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(QObject::sender());

    if (reply->error() != QNetworkReply::NoError) {
        qCDebug(TerrainQueryLog) << "_requestFinished error:" << reply->error();
        reply->deleteLater();
        _terrainData(false /* success */ , QJsonValue());
        return;
    }

    QByteArray responseBytes = reply->readAll();
    reply->deleteLater();

    // Convert the response to Json
    QJsonParseError parseError;
    QJsonDocument responseJson = QJsonDocument::fromJson(responseBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCDebug(TerrainQueryLog) << "_requestFinished unable to parse json:" << parseError.errorString();
        _terrainData(false /* success */ , QJsonValue());
        return;
    }

    // Check airmap reponse status
    QJsonObject rootObject = responseJson.object();
    QString status = rootObject["status"].toString();
    if (status != "success") {
        qCDebug(TerrainQueryLog) << "_requestFinished status != success:" << status;
        _terrainData(false /* success */ , QJsonValue());
        return;
    }

    // Send back data
    _terrainData(true /* success */ , rootObject["data"]);
}

TerrainAtCoordinateBatchManager::TerrainAtCoordinateBatchManager(void)
{
    _batchTimer.setSingleShot(true);
    _batchTimer.setInterval(_batchTimeout);
    connect(&_batchTimer, &QTimer::timeout, this, &TerrainAtCoordinateBatchManager::_sendNextBatch);
}

void TerrainAtCoordinateBatchManager::addQuery(TerrainAtCoordinateQuery* terrainAtCoordinateQuery, const QList<QGeoCoordinate>& coordinates)
{
    if (coordinates.length() > 0) {
        qCDebug(TerrainQueryLog) << "addQuery: TerrainAtCoordinateQuery:coordinates.count" << terrainAtCoordinateQuery << coordinates.count();
        connect(terrainAtCoordinateQuery, &TerrainAtCoordinateQuery::destroyed, this, &TerrainAtCoordinateBatchManager::_queryObjectDestroyed);
        QueuedRequestInfo_t queuedRequestInfo = { terrainAtCoordinateQuery, coordinates };
        _requestQueue.append(queuedRequestInfo);
        if (!_batchTimer.isActive()) {
            _batchTimer.start();
        }
    }
}

void TerrainAtCoordinateBatchManager::_sendNextBatch(void)
{
    qCDebug(TerrainQueryLog) << "_sendNextBatch _state:_requestQueue.count:_sentRequests.count" << _stateToString(_state) << _requestQueue.count() << _sentRequests.count();

    if (_state != State::Idle) {
        // Waiting for last download the complete, wait some more
        _batchTimer.start();
        return;
    }

    if (_requestQueue.count() == 0) {
        return;
    }

    _sentRequests.clear();

    // Convert coordinates to point strings for json query
    QString points;
    foreach (const QueuedRequestInfo_t& requestInfo, _requestQueue) {
        SentRequestInfo_t sentRequestInfo = { requestInfo.terrainAtCoordinateQuery, false, requestInfo.coordinates.count() };
        qCDebug(TerrainQueryLog) << "Building request: coordinate count" << requestInfo.coordinates.count();
        _sentRequests.append(sentRequestInfo);

        foreach (const QGeoCoordinate& coord, requestInfo.coordinates) {
            points += QString::number(coord.latitude(), 'f', 10) + ","
                    + QString::number(coord.longitude(), 'f', 10) + ",";
        }

    }
    points = points.mid(0, points.length() - 1); // remove the last ',' from string
    _requestQueue.clear();

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("points"), points);
    _sendQuery(QString() /* path */, query);
    _state = State::Downloading;
}

void TerrainAtCoordinateBatchManager::_batchFailed(void)
{
    QList<float>    noAltitudes;

    foreach (const SentRequestInfo_t& sentRequestInfo, _sentRequests) {
        if (!sentRequestInfo.queryObjectDestroyed) {
            disconnect(sentRequestInfo.terrainAtCoordinateQuery, &TerrainAtCoordinateQuery::destroyed, this, &TerrainAtCoordinateBatchManager::_queryObjectDestroyed);
            sentRequestInfo.terrainAtCoordinateQuery->_signalTerrainData(false, noAltitudes);
        }
    }
    _sentRequests.clear();
}

void TerrainAtCoordinateBatchManager::_queryObjectDestroyed(QObject* terrainAtCoordinateQuery)
{
    // Remove/Mark deleted objects queries from queues

    qCDebug(TerrainQueryLog) << "_TerrainAtCoordinateQueryDestroyed TerrainAtCoordinateQuery" << terrainAtCoordinateQuery;

    int i = 0;
    while (i < _requestQueue.count()) {
        const QueuedRequestInfo_t& requestInfo = _requestQueue[i];
        if (requestInfo.terrainAtCoordinateQuery == terrainAtCoordinateQuery) {
            qCDebug(TerrainQueryLog) << "Removing deleted provider from _requestQueue index:terrainAtCoordinateQuery" << i << requestInfo.terrainAtCoordinateQuery;
            _requestQueue.removeAt(i);
        } else {
            i++;
        }
    }

    for (int i=0; i<_sentRequests.count(); i++) {
        SentRequestInfo_t& sentRequestInfo = _sentRequests[i];
        if (sentRequestInfo.terrainAtCoordinateQuery == terrainAtCoordinateQuery) {
            qCDebug(TerrainQueryLog) << "Zombieing deleted provider from _sentRequests index:terrainAtCoordinateQuery" << sentRequestInfo.terrainAtCoordinateQuery;
            sentRequestInfo.queryObjectDestroyed = true;
        }
    }
}

QString TerrainAtCoordinateBatchManager::_stateToString(State state)
{
    switch (state) {
    case State::Idle:
        return QStringLiteral("Idle");
    case State::Downloading:
        return QStringLiteral("Downloading");
    }

    return QStringLiteral("State unknown");
}

void TerrainAtCoordinateBatchManager::_terrainData(bool success, const QJsonValue& dataJsonValue)
{
    _state = State::Idle;

    if (!success) {
        _batchFailed();
        return;
    }

    QList<float> altitudes;
    const QJsonArray& dataArray = dataJsonValue.toArray();
    for (int i = 0; i < dataArray.count(); i++) {
        altitudes.push_back(dataArray[i].toDouble());
    }

    int currentIndex = 0;
    foreach (const SentRequestInfo_t& sentRequestInfo, _sentRequests) {
        if (!sentRequestInfo.queryObjectDestroyed) {
            disconnect(sentRequestInfo.terrainAtCoordinateQuery, &TerrainAtCoordinateQuery::destroyed, this, &TerrainAtCoordinateBatchManager::_queryObjectDestroyed);
            QList<float> requestAltitudes = altitudes.mid(currentIndex, sentRequestInfo.cCoord);
            sentRequestInfo.terrainAtCoordinateQuery->_signalTerrainData(true, requestAltitudes);
            currentIndex += sentRequestInfo.cCoord;
        }
    }
    _sentRequests.clear();
}

TerrainAtCoordinateQuery::TerrainAtCoordinateQuery(QObject* parent)
    : QObject(parent)
{

}
void TerrainAtCoordinateQuery::requestData(const QList<QGeoCoordinate>& coordinates)
{
    if (coordinates.length() == 0) {
        return;
    }

    _TerrainAtCoordinateBatchManager->addQuery(this, coordinates);
}

void TerrainAtCoordinateQuery::_signalTerrainData(bool success, QList<float>& altitudes)
{
    emit terrainData(success, altitudes);
}

TerrainPathQuery::TerrainPathQuery(QObject* parent)
    : TerrainQuery(parent)
{
    qRegisterMetaType<PathHeightInfo_t>();
}

void TerrainPathQuery::requestData(const QGeoCoordinate& fromCoord, const QGeoCoordinate& toCoord)
{
    if (!fromCoord.isValid() || !toCoord.isValid()) {
        return;
    }

    QString points;
    points += QString::number(fromCoord.latitude(), 'f', 10) + ","
            + QString::number(fromCoord.longitude(), 'f', 10) + ",";
    points += QString::number(toCoord.latitude(), 'f', 10) + ","
            + QString::number(toCoord.longitude(), 'f', 10);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("points"), points);

    _sendQuery(QStringLiteral("/path"), query);
}

void TerrainPathQuery::_terrainData(bool success, const QJsonValue& dataJsonValue)
{
    if (!success) {
        emit terrainData(false /* success */, PathHeightInfo_t());
        return;
    }

    QJsonObject jsonObject =    dataJsonValue.toArray()[0].toObject();
    QJsonArray stepArray =      jsonObject["step"].toArray();
    QJsonArray profileArray =   jsonObject["profile"].toArray();

    PathHeightInfo_t pathHeightInfo;
    pathHeightInfo.latStep = stepArray[0].toDouble();
    pathHeightInfo.lonStep = stepArray[1].toDouble();
    foreach (const QJsonValue& profileValue, profileArray) {
        pathHeightInfo.rgHeight.append(profileValue.toDouble());
    }

    emit terrainData(true /* success */, pathHeightInfo);
}

TerrainPolyPathQuery::TerrainPolyPathQuery(QObject* parent)
    : QObject   (parent)
    , _curIndex (0)
{
    connect(&_pathQuery, &TerrainPathQuery::terrainData, this, &TerrainPolyPathQuery::_terrainDataReceived);
}

void TerrainPolyPathQuery::requestData(const QVariantList& polyPath)
{
    QList<QGeoCoordinate> path;

    foreach (const QVariant& geoVar, polyPath) {
        path.append(geoVar.value<QGeoCoordinate>());
    }

    requestData(path);
}

void TerrainPolyPathQuery::requestData(const QList<QGeoCoordinate>& polyPath)
{
    // Kick off first request
    _rgCoords = polyPath;
    _curIndex = 0;
    _pathQuery.requestData(_rgCoords[0], _rgCoords[1]);
}

void TerrainPolyPathQuery::_terrainDataReceived(bool success, const TerrainPathQuery::PathHeightInfo_t& pathHeightInfo)
{
    if (!success) {
        _rgPathHeightInfo.clear();
        emit terrainData(false /* success */, _rgPathHeightInfo);
        return;
    }

    _rgPathHeightInfo.append(pathHeightInfo);

    if (++_curIndex >= _rgCoords.count() - 1) {
        // We've finished all requests
        emit terrainData(true /* success */, _rgPathHeightInfo);
    } else {
        _pathQuery.requestData(_rgCoords[_curIndex], _rgCoords[_curIndex+1]);
    }
}

TerrainCarpetQuery::TerrainCarpetQuery(QObject* parent)
    : TerrainQuery(parent)
{

}

void TerrainCarpetQuery::requestData(const QGeoCoordinate& swCoord, const QGeoCoordinate& neCoord, bool statsOnly)
{
    if (!swCoord.isValid() || !neCoord.isValid()) {
        return;
    }

    _statsOnly = statsOnly;

    QString points;
    points += QString::number(swCoord.latitude(), 'f', 10) + ","
            + QString::number(swCoord.longitude(), 'f', 10) + ",";
    points += QString::number(neCoord.latitude(), 'f', 10) + ","
            + QString::number(neCoord.longitude(), 'f', 10);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("points"), points);

    _sendQuery(QStringLiteral("/carpet"), query);
}

void TerrainCarpetQuery::_terrainData(bool success, const QJsonValue& dataJsonValue)
{
    if (!success) {
        emit terrainData(false /* success */, qQNaN() /* minHeight */ , qQNaN() /* maxHeight */, QList<QList<double>>() /* carpet */);
        return;
    }

    qDebug() << dataJsonValue;

    QJsonObject jsonObject =    dataJsonValue.toArray()[0].toObject();

    QJsonObject statsObject =   jsonObject["stats"].toObject();
    double      minHeight =     statsObject["min"].toDouble();
    double      maxHeight =     statsObject["min"].toDouble();

    QList<QList<double>> carpet;
    if (!_statsOnly) {
        QJsonArray carpetArray =   jsonObject["carpet"].toArray();

        for (int i=0; i<carpetArray.count(); i++) {
            QJsonArray rowArray = carpetArray[i].toArray();
            carpet.append(QList<double>());

            for (int j=0; j<rowArray.count(); j++) {
                double height = rowArray[j].toDouble();
                carpet.last().append(height);
            }
        }
    }

    emit terrainData(true /*success*/, minHeight, maxHeight, carpet);
}
