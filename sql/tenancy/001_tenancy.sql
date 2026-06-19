-- cellar — tenancy (pooled multi-tenant mode).
--
-- Pooled-only: single-tenant (Model A) deployments never apply this set, so
-- cel_users has no tenant_id there and tenant scoping stays off. Apply with
-- `cellar migrate --tenancy`.
--
-- This creates the tenant registry and links users to a tenant. A business
-- table opts into tenant scoping simply by carrying the configured tenant column
-- (CEL_TENANT_COLUMN, conventionally tenant_id) — the engine auto-scopes any
-- catalog table that has it.
CREATE EXTENSION IF NOT EXISTS pgcrypto;   -- gen_random_uuid()

CREATE TABLE IF NOT EXISTS cel_tenants (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name        TEXT NOT NULL,
    is_active   BOOLEAN NOT NULL DEFAULT true,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Link every user to a tenant. Platform admins (global) leave this NULL.
ALTER TABLE cel_users ADD COLUMN IF NOT EXISTS tenant_id UUID REFERENCES cel_tenants(id);
CREATE INDEX IF NOT EXISTS cel_users_tenant_idx ON cel_users(tenant_id);
