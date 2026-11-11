import { createRootRoute, Navigate, Outlet } from '@tanstack/react-router'

export const Route = createRootRoute({
  component: Outlet,
  notFoundComponent: () => <Navigate to="/filament_manager" replace />,
})
