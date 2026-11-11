/* eslint-disable */

// @ts-nocheck

// Hand-maintained: the route-generation plugin is not part of this build.
// Keep in step with the files under ./routes.

import { Route as rootRoute } from './routes/__root'
import { Route as FilamentmanagerImport } from './routes/filament_manager'
import { Route as IndexImport } from './routes/index'

const FilamentmanagerRoute = FilamentmanagerImport.update({
  id: '/filament_manager',
  path: '/filament_manager',
  getParentRoute: () => rootRoute,
} as any)

const IndexRoute = IndexImport.update({
  id: '/',
  path: '/',
  getParentRoute: () => rootRoute,
} as any)

declare module '@tanstack/react-router' {
  interface FileRoutesByPath {
    '/': {
      id: '/'
      path: '/'
      fullPath: '/'
      preLoaderRoute: typeof IndexImport
      parentRoute: typeof rootRoute
    }
    '/filament_manager': {
      id: '/filament_manager'
      path: '/filament_manager'
      fullPath: '/filament_manager'
      preLoaderRoute: typeof FilamentmanagerImport
      parentRoute: typeof rootRoute
    }
  }
}

export interface FileRoutesByFullPath {
  '/': typeof IndexRoute
  '/filament_manager': typeof FilamentmanagerRoute
}

export interface FileRoutesByTo {
  '/': typeof IndexRoute
  '/filament_manager': typeof FilamentmanagerRoute
}

export interface FileRoutesById {
  __root__: typeof rootRoute
  '/': typeof IndexRoute
  '/filament_manager': typeof FilamentmanagerRoute
}

export interface FileRouteTypes {
  fileRoutesByFullPath: FileRoutesByFullPath
  fullPaths: '/' | '/filament_manager'
  fileRoutesByTo: FileRoutesByTo
  to: '/' | '/filament_manager'
  id: '__root__' | '/' | '/filament_manager'
  fileRoutesById: FileRoutesById
}

export interface RootRouteChildren {
  IndexRoute: typeof IndexRoute
  FilamentmanagerRoute: typeof FilamentmanagerRoute
}

const rootRouteChildren: RootRouteChildren = {
  IndexRoute: IndexRoute,
  FilamentmanagerRoute: FilamentmanagerRoute,
}

export const routeTree = rootRoute
  ._addFileChildren(rootRouteChildren)
  ._addFileTypes<FileRouteTypes>()
