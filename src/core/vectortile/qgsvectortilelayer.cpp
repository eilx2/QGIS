/***************************************************************************
  qgsvectortilelayer.cpp
  --------------------------------------
  Date                 : March 2020
  Copyright            : (C) 2020 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsvectortilelayer.h"

#include "qgslogger.h"
#include "qgsvectortilelayerrenderer.h"
#include "qgsmbtiles.h"
#include "qgsvectortilebasiclabeling.h"
#include "qgsvectortilebasicrenderer.h"
#include "qgsvectortilelabeling.h"
#include "qgsvectortileloader.h"
#include "qgsvectortileutils.h"
#include "qgsnetworkaccessmanager.h"

#include "qgsdatasourceuri.h"
#include "qgslayermetadataformatter.h"
#include "qgsblockingnetworkrequest.h"
#include "qgsmapboxglstyleconverter.h"

QgsVectorTileLayer::QgsVectorTileLayer( const QString &uri, const QString &baseName )
  : QgsMapLayer( QgsMapLayerType::VectorTileLayer, baseName )
{
  mDataSource = uri;

  setValid( loadDataSource() );

  // set a default renderer
  QgsVectorTileBasicRenderer *renderer = new QgsVectorTileBasicRenderer;
  renderer->setStyles( QgsVectorTileBasicRenderer::simpleStyleWithRandomColors() );
  setRenderer( renderer );
}

bool QgsVectorTileLayer::loadDataSource()
{
  QgsDataSourceUri dsUri;
  dsUri.setEncodedUri( mDataSource );

  mSourceType = dsUri.param( QStringLiteral( "type" ) );
  mSourcePath = dsUri.param( QStringLiteral( "url" ) );
  if ( mSourceType == QStringLiteral( "xyz" ) && dsUri.param( QStringLiteral( "serviceType" ) ) == QLatin1String( "arcgis" ) )
  {
    if ( !setupArcgisVectorTileServiceConnection( mSourcePath ) )
      return false;
  }
  else if ( mSourceType == QStringLiteral( "xyz" ) )
  {
    if ( !QgsVectorTileUtils::checkXYZUrlTemplate( mSourcePath ) )
    {
      QgsDebugMsg( QStringLiteral( "Invalid format of URL for XYZ source: " ) + mSourcePath );
      return false;
    }

    // online tiles
    mSourceMinZoom = 0;
    mSourceMaxZoom = 14;

    if ( dsUri.hasParam( QStringLiteral( "zmin" ) ) )
      mSourceMinZoom = dsUri.param( QStringLiteral( "zmin" ) ).toInt();
    if ( dsUri.hasParam( QStringLiteral( "zmax" ) ) )
      mSourceMaxZoom = dsUri.param( QStringLiteral( "zmax" ) ).toInt();

    setExtent( QgsRectangle( -20037508.3427892, -20037508.3427892, 20037508.3427892, 20037508.3427892 ) );
  }
  else if ( mSourceType == QStringLiteral( "mbtiles" ) )
  {
    QgsMbTiles reader( mSourcePath );
    if ( !reader.open() )
    {
      QgsDebugMsg( QStringLiteral( "failed to open MBTiles file: " ) + mSourcePath );
      return false;
    }

    QString format = reader.metadataValue( QStringLiteral( "format" ) );
    if ( format != QStringLiteral( "pbf" ) )
    {
      QgsDebugMsg( QStringLiteral( "Cannot open MBTiles for vector tiles. Format = " ) + format );
      return false;
    }

    QgsDebugMsgLevel( QStringLiteral( "name: " ) + reader.metadataValue( QStringLiteral( "name" ) ), 2 );
    bool minZoomOk, maxZoomOk;
    int minZoom = reader.metadataValue( QStringLiteral( "minzoom" ) ).toInt( &minZoomOk );
    int maxZoom = reader.metadataValue( QStringLiteral( "maxzoom" ) ).toInt( &maxZoomOk );
    if ( minZoomOk )
      mSourceMinZoom = minZoom;
    if ( maxZoomOk )
      mSourceMaxZoom = maxZoom;
    QgsDebugMsgLevel( QStringLiteral( "zoom range: %1 - %2" ).arg( mSourceMinZoom ).arg( mSourceMaxZoom ), 2 );

    QgsRectangle r = reader.extent();
    QgsCoordinateTransform ct( QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:4326" ) ),
                               QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:3857" ) ), transformContext() );
    r = ct.transformBoundingBox( r );
    setExtent( r );
  }
  else
  {
    QgsDebugMsg( QStringLiteral( "Unknown source type: " ) + mSourceType );
    return false;
  }

  setCrs( QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:3857" ) ) );
  return true;
}

bool QgsVectorTileLayer::setupArcgisVectorTileServiceConnection( const QString &uri )
{
  QNetworkRequest request = QNetworkRequest( QUrl( uri ) );

  QgsSetRequestInitiatorClass( request, QStringLiteral( "QgsVectorTileLayer" ) );

  QgsBlockingNetworkRequest networkRequest;
  switch ( networkRequest.get( request ) )
  {
    case QgsBlockingNetworkRequest::NoError:
      break;

    case QgsBlockingNetworkRequest::NetworkError:
    case QgsBlockingNetworkRequest::TimeoutError:
    case QgsBlockingNetworkRequest::ServerExceptionError:
      return false;
  }

  const QgsNetworkReplyContent content = networkRequest.reply();
  const QByteArray raw = content.content();

  // Parse data
  QJsonParseError err;
  QJsonDocument doc = QJsonDocument::fromJson( raw, &err );
  if ( doc.isNull() )
  {
    return false;
  }
  mArcgisLayerConfiguration = doc.object().toVariantMap();
  if ( mArcgisLayerConfiguration.contains( QStringLiteral( "error" ) ) )
  {
    return false;
  }

  mArcgisLayerConfiguration.insert( QStringLiteral( "serviceUri" ), uri );
  mSourcePath = uri + '/' + mArcgisLayerConfiguration.value( QStringLiteral( "tiles" ) ).toList().value( 0 ).toString();
  if ( !QgsVectorTileUtils::checkXYZUrlTemplate( mSourcePath ) )
  {
    QgsDebugMsg( QStringLiteral( "Invalid format of URL for XYZ source: " ) + mSourcePath );
    return false;
  }

  mSourceMinZoom = 0;
  mSourceMaxZoom = mArcgisLayerConfiguration.value( QStringLiteral( "maxzoom" ) ).toInt();
  setExtent( QgsRectangle( -20037508.3427892, -20037508.3427892, 20037508.3427892, 20037508.3427892 ) );

  return true;
}

QgsVectorTileLayer::~QgsVectorTileLayer() = default;


QgsVectorTileLayer *QgsVectorTileLayer::clone() const
{
  QgsVectorTileLayer *layer = new QgsVectorTileLayer( source(), name() );
  layer->setRenderer( renderer() ? renderer()->clone() : nullptr );
  return layer;
}

QgsMapLayerRenderer *QgsVectorTileLayer::createMapRenderer( QgsRenderContext &rendererContext )
{
  return new QgsVectorTileLayerRenderer( this, rendererContext );
}

bool QgsVectorTileLayer::readXml( const QDomNode &layerNode, QgsReadWriteContext &context )
{
  setValid( loadDataSource() );

  QString errorMsg;
  if ( !readSymbology( layerNode, errorMsg, context ) )
    return false;

  readStyleManager( layerNode );
  return true;
}

bool QgsVectorTileLayer::writeXml( QDomNode &layerNode, QDomDocument &doc, const QgsReadWriteContext &context ) const
{
  QDomElement mapLayerNode = layerNode.toElement();
  mapLayerNode.setAttribute( QStringLiteral( "type" ), QStringLiteral( "vector-tile" ) );

  writeStyleManager( layerNode, doc );

  QString errorMsg;
  return writeSymbology( layerNode, doc, errorMsg, context );
}

bool QgsVectorTileLayer::readSymbology( const QDomNode &node, QString &errorMessage, QgsReadWriteContext &context, QgsMapLayer::StyleCategories categories )
{
  QDomElement elem = node.toElement();

  readCommonStyle( elem, context, categories );

  const QDomElement elemRenderer = elem.firstChildElement( QStringLiteral( "renderer" ) );
  if ( elemRenderer.isNull() )
  {
    errorMessage = tr( "Missing <renderer> tag" );
    return false;
  }
  const QString rendererType = elemRenderer.attribute( QStringLiteral( "type" ) );

  if ( categories.testFlag( Symbology ) )
  {
    QgsVectorTileRenderer *r = nullptr;
    if ( rendererType == QStringLiteral( "basic" ) )
      r = new QgsVectorTileBasicRenderer;
    else
    {
      errorMessage = tr( "Unknown renderer type: " ) + rendererType;
      return false;
    }

    r->readXml( elemRenderer, context );
    setRenderer( r );
  }

  if ( categories.testFlag( Labeling ) )
  {
    setLabeling( nullptr );
    const QDomElement elemLabeling = elem.firstChildElement( QStringLiteral( "labeling" ) );
    if ( !elemLabeling.isNull() )
    {
      const QString labelingType = elemLabeling.attribute( QStringLiteral( "type" ) );
      QgsVectorTileLabeling *labeling = nullptr;
      if ( labelingType == QStringLiteral( "basic" ) )
        labeling = new QgsVectorTileBasicLabeling;
      else
      {
        errorMessage = tr( "Unknown labeling type: " ) + rendererType;
      }

      if ( labeling )
      {
        labeling->readXml( elemLabeling, context );
        setLabeling( labeling );
      }
    }
  }

  return true;
}

bool QgsVectorTileLayer::writeSymbology( QDomNode &node, QDomDocument &doc, QString &errorMessage, const QgsReadWriteContext &context, QgsMapLayer::StyleCategories categories ) const
{
  Q_UNUSED( errorMessage )
  QDomElement elem = node.toElement();

  writeCommonStyle( elem, doc, context, categories );

  if ( mRenderer )
  {
    QDomElement elemRenderer = doc.createElement( QStringLiteral( "renderer" ) );
    elemRenderer.setAttribute( QStringLiteral( "type" ), mRenderer->type() );
    if ( categories.testFlag( Symbology ) )
    {
      mRenderer->writeXml( elemRenderer, context );
    }
    elem.appendChild( elemRenderer );
  }

  if ( mLabeling && categories.testFlag( Labeling ) )
  {
    QDomElement elemLabeling = doc.createElement( QStringLiteral( "labeling" ) );
    elemLabeling.setAttribute( QStringLiteral( "type" ), mLabeling->type() );
    mLabeling->writeXml( elemLabeling, context );
    elem.appendChild( elemLabeling );
  }

  return true;
}

void QgsVectorTileLayer::setTransformContext( const QgsCoordinateTransformContext &transformContext )
{
  Q_UNUSED( transformContext )
}

QString QgsVectorTileLayer::loadDefaultStyle( bool &resultFlag )
{
  QgsDataSourceUri dsUri;
  dsUri.setEncodedUri( mDataSource );
  if ( mSourceType == QStringLiteral( "xyz" ) && dsUri.param( QStringLiteral( "serviceType" ) ) == QLatin1String( "arcgis" ) )
  {
    QNetworkRequest request = QNetworkRequest( QUrl( mArcgisLayerConfiguration.value( QStringLiteral( "serviceUri" ) ).toString()
                              + '/' + mArcgisLayerConfiguration.value( QStringLiteral( "defaultStyles" ) ).toString() ) );

    QgsSetRequestInitiatorClass( request, QStringLiteral( "QgsVectorTileLayer" ) );

    QgsBlockingNetworkRequest networkRequest;
    switch ( networkRequest.get( request ) )
    {
      case QgsBlockingNetworkRequest::NoError:
        break;

      case QgsBlockingNetworkRequest::NetworkError:
      case QgsBlockingNetworkRequest::TimeoutError:
      case QgsBlockingNetworkRequest::ServerExceptionError:
        resultFlag = false;
        return QObject::tr( "Error retrieving default style" );
    }

    const QgsNetworkReplyContent content = networkRequest.reply();
    const QByteArray raw = content.content();

    QgsMapBoxGlStyleConversionContext context;
    // convert automatically from pixel sizes to millimeters, because pixel sizes
    // are a VERY edge case in QGIS and don't play nice with hidpi map renders or print layouts
    context.setTargetUnit( QgsUnitTypes::RenderMillimeters );
    //assume source uses 96 dpi
    context.setPixelSizeConversionFactor( 25.4 / 96.0 );

    QgsMapBoxGlStyleConverter converter;
    if ( converter.convert( raw, &context ) != QgsMapBoxGlStyleConverter::Success )
    {
      resultFlag = false;
      return converter.errorMessage();
    }

    setRenderer( converter.renderer() );
    setLabeling( converter.labeling() );
    resultFlag = true;
    return QString();
  }
  else
  {
    QgsMapLayer::loadDefaultStyle( resultFlag );
    resultFlag = true;
    return QString();
  }
}

QString QgsVectorTileLayer::loadDefaultMetadata( bool &resultFlag )
{
  QgsDataSourceUri dsUri;
  dsUri.setEncodedUri( mDataSource );
  if ( mSourceType == QStringLiteral( "xyz" ) && dsUri.param( QStringLiteral( "serviceType" ) ) == QLatin1String( "arcgis" ) )
  {
    // populate default metadata
    QgsLayerMetadata metadata;
    metadata.setIdentifier( mArcgisLayerConfiguration.value( QStringLiteral( "serviceUri" ) ).toString() );
    const QString parentIdentifier = mArcgisLayerConfiguration.value( QStringLiteral( "serviceItemId" ) ).toString();
    if ( !parentIdentifier.isEmpty() )
    {
      metadata.setParentIdentifier( parentIdentifier );
    }
    metadata.setType( QStringLiteral( "dataset" ) );
    metadata.setTitle( mArcgisLayerConfiguration.value( QStringLiteral( "name" ) ).toString() );
    QString copyright = mArcgisLayerConfiguration.value( QStringLiteral( "copyrightText" ) ).toString();
    if ( !copyright.isEmpty() )
      metadata.setRights( QStringList() << copyright );
    metadata.addLink( QgsAbstractMetadataBase::Link( tr( "Source" ), QStringLiteral( "WWW:LINK" ), mArcgisLayerConfiguration.value( QStringLiteral( "serviceUri" ) ).toString() ) );

    setMetadata( metadata );

    resultFlag = true;
    return QString();
  }
  else
  {
    QgsMapLayer::loadDefaultMetadata( resultFlag );
    resultFlag = true;
    return QString();
  }
}

QString QgsVectorTileLayer::encodedSource( const QString &source, const QgsReadWriteContext &context ) const
{
  QgsDataSourceUri dsUri;
  dsUri.setEncodedUri( source );

  QString sourceType = dsUri.param( QStringLiteral( "type" ) );
  QString sourcePath = dsUri.param( QStringLiteral( "url" ) );
  if ( sourceType == QStringLiteral( "xyz" ) )
  {
    QUrl sourceUrl( sourcePath );
    if ( sourceUrl.isLocalFile() )
    {
      // relative path will become "file:./x.txt"
      QString relSrcUrl = context.pathResolver().writePath( sourceUrl.toLocalFile() );
      dsUri.removeParam( QStringLiteral( "url" ) );  // needed because setParam() would insert second "url" key
      dsUri.setParam( QStringLiteral( "url" ), QUrl::fromLocalFile( relSrcUrl ).toString() );
      return dsUri.encodedUri();
    }
  }
  else if ( sourceType == QStringLiteral( "mbtiles" ) )
  {
    sourcePath = context.pathResolver().writePath( sourcePath );
    dsUri.removeParam( QStringLiteral( "url" ) );  // needed because setParam() would insert second "url" key
    dsUri.setParam( QStringLiteral( "url" ), sourcePath );
    return dsUri.encodedUri();
  }

  return source;
}

QString QgsVectorTileLayer::decodedSource( const QString &source, const QString &provider, const QgsReadWriteContext &context ) const
{
  Q_UNUSED( provider )

  QgsDataSourceUri dsUri;
  dsUri.setEncodedUri( source );

  QString sourceType = dsUri.param( QStringLiteral( "type" ) );
  QString sourcePath = dsUri.param( QStringLiteral( "url" ) );
  if ( sourceType == QStringLiteral( "xyz" ) )
  {
    QUrl sourceUrl( sourcePath );
    if ( sourceUrl.isLocalFile() )  // file-based URL? convert to relative path
    {
      QString absSrcUrl = context.pathResolver().readPath( sourceUrl.toLocalFile() );
      dsUri.removeParam( QStringLiteral( "url" ) );  // needed because setParam() would insert second "url" key
      dsUri.setParam( QStringLiteral( "url" ), QUrl::fromLocalFile( absSrcUrl ).toString() );
      return dsUri.encodedUri();
    }
  }
  else if ( sourceType == QStringLiteral( "mbtiles" ) )
  {
    sourcePath = context.pathResolver().readPath( sourcePath );
    dsUri.removeParam( QStringLiteral( "url" ) );  // needed because setParam() would insert second "url" key
    dsUri.setParam( QStringLiteral( "url" ), sourcePath );
    return dsUri.encodedUri();
  }

  return source;
}

QString QgsVectorTileLayer::htmlMetadata() const
{
  QgsLayerMetadataFormatter htmlFormatter( metadata() );

  QString info = QStringLiteral( "<html><head></head>\n<body>\n" );

  info += QStringLiteral( "<h1>" ) + tr( "Information from provider" ) + QStringLiteral( "</h1>\n<hr>\n" ) %
          QStringLiteral( "<table class=\"list-view\">\n" ) %

          // name
          QStringLiteral( "<tr><td class=\"highlight\">" ) % tr( "Name" ) % QStringLiteral( "</td><td>" ) % name() % QStringLiteral( "</td></tr>\n" );

  info += QStringLiteral( "<tr><td class=\"highlight\">" ) % tr( "URI" ) % QStringLiteral( "</td><td>" ) % source() % QStringLiteral( "</td></tr>\n" );
  info += QStringLiteral( "<tr><td class=\"highlight\">" ) % tr( "Source type" ) % QStringLiteral( "</td><td>" ) % sourceType() % QStringLiteral( "</td></tr>\n" );

  const QString url = sourcePath();
  info += QStringLiteral( "<tr><td class=\"highlight\">" ) % tr( "Source path" ) % QStringLiteral( "</td><td>%1" ).arg( QStringLiteral( "<a href=\"%1\">%2</a>" ).arg( QUrl( url ).toString(), sourcePath() ) ) + QStringLiteral( "</td></tr>\n" );

  info += QStringLiteral( "<tr><td class=\"highlight\">" ) % tr( "Zoom levels" ) % QStringLiteral( "</td><td>" ) % QStringLiteral( "%1 - %2" ).arg( sourceMinZoom() ).arg( sourceMaxZoom() ) % QStringLiteral( "</td></tr>\n" );
  info += QStringLiteral( "</table>" );

  // End Provider section
  info += QStringLiteral( "</table>\n<br><br>" );

  // Identification section
  info += QStringLiteral( "<h1>" ) % tr( "Identification" ) % QStringLiteral( "</h1>\n<hr>\n" ) %
          htmlFormatter.identificationSectionHtml() %
          QStringLiteral( "<br><br>\n" ) %

          // extent section
          QStringLiteral( "<h1>" ) % tr( "Extent" ) % QStringLiteral( "</h1>\n<hr>\n" ) %
          htmlFormatter.extentSectionHtml( ) %
          QStringLiteral( "<br><br>\n" ) %

          // Start the Access section
          QStringLiteral( "<h1>" ) % tr( "Access" ) % QStringLiteral( "</h1>\n<hr>\n" ) %
          htmlFormatter.accessSectionHtml( ) %
          QStringLiteral( "<br><br>\n" ) %


          // Start the contacts section
          QStringLiteral( "<h1>" ) % tr( "Contacts" ) % QStringLiteral( "</h1>\n<hr>\n" ) %
          htmlFormatter.contactsSectionHtml( ) %
          QStringLiteral( "<br><br>\n" ) %

          // Start the links section
          QStringLiteral( "<h1>" ) % tr( "References" ) % QStringLiteral( "</h1>\n<hr>\n" ) %
          htmlFormatter.linksSectionHtml( ) %
          QStringLiteral( "<br><br>\n" ) %

          // Start the history section
          QStringLiteral( "<h1>" ) % tr( "History" ) % QStringLiteral( "</h1>\n<hr>\n" ) %
          htmlFormatter.historySectionHtml( ) %
          QStringLiteral( "<br><br>\n" ) %

          QStringLiteral( "\n</body>\n</html>\n" );

  return info;
}

QByteArray QgsVectorTileLayer::getRawTile( QgsTileXYZ tileID )
{
  QgsTileMatrix tileMatrix = QgsTileMatrix::fromWebMercator( tileID.zoomLevel() );
  QgsTileRange tileRange( tileID.column(), tileID.column(), tileID.row(), tileID.row() );

  QgsDataSourceUri dsUri;
  dsUri.setEncodedUri( mDataSource );
  const QString authcfg = dsUri.authConfigId();
  const QString referer = dsUri.param( QStringLiteral( "referer" ) );

  QList<QgsVectorTileRawData> rawTiles = QgsVectorTileLoader::blockingFetchTileRawData( mSourceType, mSourcePath, tileMatrix, QPointF(), tileRange, authcfg, referer );
  if ( rawTiles.isEmpty() )
    return QByteArray();
  return rawTiles.first().data;
}

void QgsVectorTileLayer::setRenderer( QgsVectorTileRenderer *r )
{
  mRenderer.reset( r );
  triggerRepaint();
}

QgsVectorTileRenderer *QgsVectorTileLayer::renderer() const
{
  return mRenderer.get();
}

void QgsVectorTileLayer::setLabeling( QgsVectorTileLabeling *labeling )
{
  mLabeling.reset( labeling );
  triggerRepaint();
}

QgsVectorTileLabeling *QgsVectorTileLayer::labeling() const
{
  return mLabeling.get();
}
