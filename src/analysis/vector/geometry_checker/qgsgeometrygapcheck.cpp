/***************************************************************************
    qgsgeometrygapcheck.cpp
    ---------------------
    begin                : September 2015
    copyright            : (C) 2014 by Sandro Mani / Sourcepole AG
    email                : smani at sourcepole dot ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsgeometryengine.h"
#include "qgsgeometrygapcheck.h"
#include "qgsgeometrycollection.h"
#include "qgsfeaturepool.h"
#include "qgsvectorlayer.h"

#include "geos_c.h"

void QgsGeometryGapCheck::collectErrors( QList<QgsGeometryCheckError *> &errors, QStringList &messages, QAtomicInt *progressCounter, const QMap<QString, QgsFeatureIds> &ids ) const
{
  if ( progressCounter )
    progressCounter->fetchAndAddRelaxed( 1 );

  QVector<QgsAbstractGeometry *> geomList;

  QMap<QString, QgsFeatureIds> featureIds = ids.isEmpty() ? allLayerFeatureIds() : ids;
  const QgsGeometryCheckerUtils::LayerFeatures layerFeatures( mContext->featurePools, featureIds, mCompatibleGeometryTypes, nullptr, mContext, true );
  for ( const QgsGeometryCheckerUtils::LayerFeature &layerFeature : layerFeatures )
  {
    geomList.append( layerFeature.geometry().constGet()->clone() );
  }

  if ( geomList.isEmpty() )
  {
    return;
  }

  std::unique_ptr< QgsGeometryEngine > geomEngine = QgsGeometryCheckerUtils::createGeomEngine( nullptr, mContext->tolerance );

  // Create union of geometry
  QString errMsg;
  std::unique_ptr<QgsAbstractGeometry> unionGeom( geomEngine->combine( geomList, &errMsg ) );
  qDeleteAll( geomList );
  if ( !unionGeom )
  {
    messages.append( tr( "Gap check: %1" ).arg( errMsg ) );
    return;
  }

  // Get envelope of union
  geomEngine = QgsGeometryCheckerUtils::createGeomEngine( unionGeom.get(), mContext->tolerance );
  std::unique_ptr<QgsAbstractGeometry> envelope( geomEngine->envelope( &errMsg ) );
  if ( !envelope )
  {
    messages.append( tr( "Gap check: %1" ).arg( errMsg ) );
    return;
  }

  // Buffer envelope
  geomEngine = QgsGeometryCheckerUtils::createGeomEngine( envelope.get(), mContext->tolerance );
  QgsAbstractGeometry *bufEnvelope = geomEngine->buffer( 2, 0, GEOSBUF_CAP_SQUARE, GEOSBUF_JOIN_MITRE, 4. );  //#spellok  //#spellok
  envelope.reset( bufEnvelope );

  // Compute difference between envelope and union to obtain gap polygons
  geomEngine = QgsGeometryCheckerUtils::createGeomEngine( envelope.get(), mContext->tolerance );
  std::unique_ptr<QgsAbstractGeometry> diffGeom( geomEngine->difference( unionGeom.get(), &errMsg ) );
  if ( !diffGeom )
  {
    messages.append( tr( "Gap check: %1" ).arg( errMsg ) );
    return;
  }

  // For each gap polygon which does not lie on the boundary, get neighboring polygons and add error
  for ( int iPart = 0, nParts = diffGeom->partCount(); iPart < nParts; ++iPart )
  {
    std::unique_ptr<QgsAbstractGeometry> gapGeom( QgsGeometryCheckerUtils::getGeomPart( diffGeom.get(), iPart )->clone() );
    // Skip the gap between features and boundingbox
    if ( gapGeom->boundingBox() == envelope->boundingBox() )
    {
      continue;
    }

    // Skip gaps above threshold
    if ( gapGeom->area() > mThresholdMapUnits || gapGeom->area() < mContext->reducedTolerance )
    {
      continue;
    }

    QgsRectangle gapAreaBBox = gapGeom->boundingBox();

    // Get neighboring polygons
    QMap<QString, QgsFeatureIds> neighboringIds;
    const QgsGeometryCheckerUtils::LayerFeatures layerFeatures( mContext->featurePools, featureIds.keys(), gapAreaBBox, mCompatibleGeometryTypes, mContext );
    for ( const QgsGeometryCheckerUtils::LayerFeature &layerFeature : layerFeatures )
    {
      if ( QgsGeometryCheckerUtils::sharedEdgeLength( gapGeom.get(), layerFeature.geometry().constGet(), mContext->reducedTolerance ) > 0 )
      {
        neighboringIds[layerFeature.layer()->id()].insert( layerFeature.feature().id() );
        gapAreaBBox.combineExtentWith( layerFeature.geometry().constGet()->boundingBox() );
      }
    }

    if ( neighboringIds.isEmpty() )
    {
      continue;
    }

    // Add error
    double area = gapGeom->area();
    errors.append( new QgsGeometryGapCheckError( this, QString(), QgsGeometry( gapGeom.release() ), neighboringIds, area, gapAreaBBox ) );

  }
}

void QgsGeometryGapCheck::fixError( QgsGeometryCheckError *error, int method, const QMap<QString, int> & /*mergeAttributeIndices*/, Changes &changes ) const
{
  if ( method == NoChange )
  {
    error->setFixed( method );
  }
  else if ( method == MergeLongestEdge )
  {
    QString errMsg;
    if ( mergeWithNeighbor( static_cast<QgsGeometryGapCheckError *>( error ), changes, errMsg ) )
    {
      error->setFixed( method );
    }
    else
    {
      error->setFixFailed( tr( "Failed to merge with neighbor: %1" ).arg( errMsg ) );
    }
  }
  else
  {
    error->setFixFailed( tr( "Unknown method" ) );
  }
}

bool QgsGeometryGapCheck::mergeWithNeighbor( QgsGeometryGapCheckError *err, Changes &changes, QString &errMsg ) const
{
  double maxVal = 0.;
  QString mergeLayerId;
  QgsFeature mergeFeature;
  int mergePartIdx = -1;

  const QgsAbstractGeometry *errGeometry = QgsGeometryCheckerUtils::getGeomPart( err->geometry(), 0 );

  const auto layerIds = err->neighbors().keys();
  // Search for touching neighboring geometries
  for ( const QString &layerId : layerIds )
  {
    QgsFeaturePool *featurePool = mContext->featurePools.value( layerId );
    std::unique_ptr<QgsAbstractGeometry> errLayerGeom( errGeometry->clone() );
    errLayerGeom->transform( mContext->layerTransform( featurePool->layer() ), QgsCoordinateTransform::ReverseTransform );

    const auto featureIds = err->neighbors().value( layerId );

    for ( QgsFeatureId testId : featureIds )
    {
      QgsFeature testFeature;
      if ( !featurePool->getFeature( testId, testFeature ) )
      {
        continue;
      }
      QgsGeometry featureGeom = testFeature.geometry();
      const QgsAbstractGeometry *testGeom = featureGeom.constGet();
      for ( int iPart = 0, nParts = testGeom->partCount(); iPart < nParts; ++iPart )
      {
        double len = QgsGeometryCheckerUtils::sharedEdgeLength( errLayerGeom.get(), QgsGeometryCheckerUtils::getGeomPart( testGeom, iPart ), mContext->reducedTolerance );
        if ( len > maxVal )
        {
          maxVal = len;
          mergeFeature = testFeature;
          mergePartIdx = iPart;
          mergeLayerId = layerId;
        }
      }
    }
  }

  if ( maxVal == 0. )
  {
    return false;
  }

  // Merge geometries
  QgsFeaturePool *featurePool = mContext->featurePools[ mergeLayerId ];
  std::unique_ptr<QgsAbstractGeometry> errLayerGeom( errGeometry->clone() );
  errLayerGeom->transform( mContext->layerTransform( featurePool->layer() ), QgsCoordinateTransform::ReverseTransform );
  QgsGeometry mergeFeatureGeom = mergeFeature.geometry();
  const QgsAbstractGeometry *mergeGeom = mergeFeatureGeom.constGet();
  std::unique_ptr< QgsGeometryEngine > geomEngine = QgsGeometryCheckerUtils::createGeomEngine( errLayerGeom.get(), mContext->reducedTolerance );
  std::unique_ptr<QgsAbstractGeometry> combinedGeom( geomEngine->combine( QgsGeometryCheckerUtils::getGeomPart( mergeGeom, mergePartIdx ), &errMsg ) );
  if ( !combinedGeom || combinedGeom->isEmpty() || !QgsWkbTypes::isSingleType( combinedGeom->wkbType() ) )
  {
    return false;
  }

  // Add merged polygon to destination geometry
  replaceFeatureGeometryPart( mergeLayerId, mergeFeature, mergePartIdx, combinedGeom.release(), changes );

  return true;
}


QStringList QgsGeometryGapCheck::resolutionMethods() const
{
  static QStringList methods = QStringList() << tr( "Add gap area to neighboring polygon with longest shared edge" ) << tr( "No action" );
  return methods;
}
