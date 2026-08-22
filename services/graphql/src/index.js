const express = require('express');
const cors = require('cors');
const { ApolloServer } = require('@apollo/server');
const { expressMiddleware } = require('@apollo/server/express4');
const { GraphQLError } = require('graphql');
const DataLoader = require('dataloader');
const { Pool } = require('pg');
const jwt = require('jsonwebtoken');
const promClient = require('prom-client');
const crypto = require('crypto');
require('dotenv').config();

const PORT = process.env.PORT || 4000;
if (
  !process.env.JWT_SECRET ||
  process.env.JWT_SECRET === 'default_super_secure_jwt_secret_key_12345_67890'
) {
  console.error(
    JSON.stringify({
      level: 'CRITICAL',
      service: 'graphql-service',
      message:
        'FATAL: JWT_SECRET is missing, empty, or a known insecure default. Set a strong random value (e.g. `make setup` or `openssl rand -hex 64`).'
    })
  );
  process.exit(1);
}
const JWT_SECRET = process.env.JWT_SECRET;
const DATABASE_URL = process.env.DATABASE_URL || 'postgres://postgres:pass@pgbouncer:6432/postgres';
const DATABASE_READ_URL = process.env.DATABASE_READ_URL || 'postgres://postgres:pass@pgbouncer-read:6433/postgres';

// Prometheus Metrics Setup
promClient.collectDefaultMetrics({ prefix: 'graphql_' });
const httpRequestsTotal = new promClient.Counter({
  name: 'http_requests_total',
  help: 'Total HTTP requests across services',
  labelNames: ['service', 'method', 'status']
});
const httpRequestDurationSeconds = new promClient.Histogram({
  name: 'http_request_duration_seconds',
  help: 'Request latency in seconds',
  labelNames: ['service', 'method']
});

// JSON Logging Helper
const logJson = (level, message, traceId = '00000000000000000000000000000000', spanId = '0000000000000000') => {
  console.log(JSON.stringify({
    timestamp: new Date().toISOString(),
    level,
    service: 'graphql-service',
    trace_id: traceId,
    span_id: spanId,
    message
  }));
};

// Helper to convert postgres URL string if formatted as libpq key=value pair
const parseConnectionString = (str) => {
  if (str.startsWith('postgres://') || str.startsWith('postgresql://')) {
    return { connectionString: str };
  }
  const config = {};
  str.split(/\s+/).forEach((pair) => {
    const idx = pair.indexOf('=');
    if (idx > 0) {
      const k = pair.substring(0, idx);
      const v = pair.substring(idx + 1);
      if (k === 'host') config.host = v;
      if (k === 'port') config.port = parseInt(v, 10);
      if (k === 'dbname') config.database = v;
      if (k === 'user') config.user = v;
      if (k === 'password') config.password = v;
    }
  });
  return config;
};

// Connection pool configuration with bounds and timeouts
const poolConfig = {
  ...parseConnectionString(DATABASE_URL),
  max: 20,
  idleTimeoutMillis: 30000,
  connectionTimeoutMillis: 5000,
};

const readPoolConfig = {
  ...parseConnectionString(DATABASE_READ_URL),
  max: 20,
  idleTimeoutMillis: 30000,
  connectionTimeoutMillis: 5000,
};

const dbPool = new Pool(poolConfig);
const dbReadPool = new Pool(readPoolConfig);

// Circuit breaker pattern implementation in JS
class CircuitBreaker {
  constructor(failureThreshold = 5, resetTimeout = 5000) {
    this.failureThreshold = failureThreshold;
    this.resetTimeout = resetTimeout;
    this.state = 'CLOSED';
    this.consecutiveFailures = 0;
    this.lastStateChange = Date.now();
  }

  allowRequest() {
    const now = Date.now();
    if (this.state === 'OPEN') {
      if (now - this.lastStateChange >= this.resetTimeout) {
        this.state = 'HALF_OPEN';
        this.lastStateChange = now;
        return true;
      }
      return false;
    }
    return true;
  }

  recordSuccess() {
    this.consecutiveFailures = 0;
    if (this.state === 'HALF_OPEN') {
      this.state = 'CLOSED';
      this.lastStateChange = Date.now();
    }
  }

  recordFailure() {
    this.consecutiveFailures += 1;
    const now = Date.now();
    if (this.state === 'CLOSED' && this.consecutiveFailures >= this.failureThreshold) {
      this.state = 'OPEN';
      this.lastStateChange = now;
    } else if (this.state === 'HALF_OPEN') {
      this.state = 'OPEN';
      this.lastStateChange = now;
    }
  }
}

const dbCircuitBreaker = new CircuitBreaker(5, 5000);

// Safe database query wrapper with Circuit Breaker, Read/Write splitting, and pool logging
const queryDB = async (text, params, isRead = true) => {
  if (!dbCircuitBreaker.allowRequest()) {
    logJson('ERROR', 'Circuit Breaker is OPEN. Fast failing query.');
    throw new Error('Circuit Breaker OPEN - Database service unavailable');
  }

  if (isRead) {
    try {
      const res = await dbReadPool.query(text, params);
      dbCircuitBreaker.recordSuccess();
      return res;
    } catch (err) {
      logJson('WARN', `Read pool query failed (${err.message}), falling back to write pool`);
    }
  }

  try {
    const res = await dbPool.query(text, params);
    dbCircuitBreaker.recordSuccess();
    return res;
  } catch (err) {
    dbCircuitBreaker.recordFailure();
    logJson('ERROR', `Query failed: ${err.message}`);
    throw err;
  }
};

const NOTE_PERMISSION_RANK = { viewer: 1, editor: 2, owner: 3 };

// Returns the caller's permission level or null when access is denied.
const getNoteAccess = async (userId, noteId) => {
  if (!userId || !noteId) return null;
  const { rows } = await queryDB(
    `SELECT CASE
              WHEN n.owner_id = $2::uuid THEN 'owner'
              ELSE s.permission
            END AS permission
     FROM notes n
     LEFT JOIN note_shares s ON s.note_id = n.id AND s.shared_with_user_id = $2::uuid
     WHERE n.id = $1::uuid`,
    [noteId, userId]
  );
  return rows.length > 0 && rows[0].permission ? rows[0].permission : null;
};

// Throws UNAUTHENTICATED/FORBIDDEN; returns the granted level.
const assertNoteAccess = async (userId, noteId, requiredPermission = 'viewer') => {
  if (!userId) {
    throw new GraphQLError('Unauthorized', { extensions: { code: 'UNAUTHENTICATED' } });
  }
  const granted = await getNoteAccess(userId, noteId);
  if (!granted) {
    throw new GraphQLError('Note not found or access denied', {
      extensions: { code: 'FORBIDDEN' }
    });
  }
  if ((NOTE_PERMISSION_RANK[granted] || 0) < (NOTE_PERMISSION_RANK[requiredPermission] || 0)) {
    throw new GraphQLError(`Forbidden: ${requiredPermission} access required`, {
      extensions: { code: 'FORBIDDEN' }
    });
  }
  return granted;
};

// DataLoaders batching logic to prevent N+1 query problem
const createLoaders = () => ({
  userLoader: new DataLoader(async (userIds) => {
    const uniqueIds = [...new Set(userIds)];
    logJson('INFO', `[DataLoader] Batching ${userIds.length} user ID lookups (${uniqueIds.length} unique) into 1 query`);
    const { rows } = await queryDB(
      'SELECT id::text, email FROM users WHERE id = ANY($1::uuid[])',
      [uniqueIds]
    );
    const userMap = new Map(rows.map((u) => [u.id, u]));
    return userIds.map((id) => userMap.get(id) || null);
  }),

  commentsLoader: new DataLoader(async (noteIds) => {
    const uniqueIds = [...new Set(noteIds)];
    logJson('INFO', `[DataLoader] Batching ${noteIds.length} note comment lookups (${uniqueIds.length} unique) into 1 query`);
    const { rows } = await queryDB(
      `SELECT id::text, note_id::text, author_id::text, content,
              to_char(created_at, 'YYYY-MM-DD"T"HH24:MI:SS"Z"') as created_at
       FROM comments
       WHERE note_id = ANY($1::uuid[])
       ORDER BY created_at ASC`,
      [uniqueIds]
    );
    const commentsMap = new Map();
    uniqueIds.forEach((id) => commentsMap.set(id, []));
    rows.forEach((row) => {
      if (!commentsMap.has(row.note_id)) {
        commentsMap.set(row.note_id, []);
      }
      commentsMap.get(row.note_id).push(row);
    });
    return noteIds.map((id) => commentsMap.get(id) || []);
  }),
});

const typeDefs = `#graphql
  type User {
    id: ID!
    email: String
  }

  type Comment {
    id: ID!
    noteId: ID!
    content: String!
    author: User!
    createdAt: String
  }

  type Note {
    id: ID!
    title: String!
    content: String!
    ownerId: ID!
    author: User!
    comments: [Comment!]!
    createdAt: String
    permission: String
  }

  type Query {
    note(id: ID!): Note
    notes: [Note!]!
    user(id: ID!): User
    comments(noteId: ID!): [Comment!]!
    me: User
  }

  type Mutation {
    createNote(title: String!, content: String!): Note!
    createComment(noteId: ID!, content: String!): Comment!
    deleteComment(id: ID!): Boolean!
  }
`;

const resolvers = {
  Query: {
    note: async (_, { id }, context) => {
      const permission = await assertNoteAccess(context.user && context.user.id, id);
      const { rows } = await queryDB(
        `SELECT id::text, title, content, owner_id::text,
                to_char(created_at, 'YYYY-MM-DD"T"HH24:MI:SS"Z"') as created_at
         FROM notes WHERE id = $1::uuid`,
        [id]
      );
      if (rows.length === 0) return null;
      return { ...rows[0], permission };
    },

    notes: async (_, __, context) => {
      if (!context.user) {
        throw new GraphQLError('Unauthorized', { extensions: { code: 'UNAUTHENTICATED' } });
      }
      const query = `
        SELECT n.id::text, n.title, n.content, n.owner_id::text,
               to_char(n.created_at, 'YYYY-MM-DD"T"HH24:MI:SS"Z"') as created_at,
               CASE WHEN n.owner_id = $1::uuid THEN 'owner' ELSE COALESCE(s.permission, 'editor') END AS permission
        FROM notes n
        LEFT JOIN note_shares s ON n.id = s.note_id AND s.shared_with_user_id = $1::uuid
        WHERE n.owner_id = $1::uuid OR s.shared_with_user_id = $1::uuid
        ORDER BY n.id ASC
      `;
      const params = [context.user.id];
      const { rows } = await queryDB(query, params);
      return rows;
    },

    user: async (_, { id }, context) => {
      return context.loaders.userLoader.load(id);
    },

    comments: async (_, { noteId }, context) => {
      await assertNoteAccess(context.user && context.user.id, noteId);
      return context.loaders.commentsLoader.load(noteId);
    },

    me: async (_, __, context) => {
      if (!context.user) return null;
      return context.loaders.userLoader.load(context.user.id);
    },
  },

  Mutation: {
    createNote: async (_, { title, content }, context) => {
      if (!context.user) {
        throw new GraphQLError('Unauthorized', { extensions: { code: 'UNAUTHENTICATED' } });
      }
      const { rows } = await queryDB(
        `INSERT INTO notes (title, content, owner_id)
         VALUES ($1, $2, $3::uuid)
         RETURNING id::text, title, content, owner_id::text,
                   to_char(created_at, 'YYYY-MM-DD"T"HH24:MI:SS"Z"') as created_at`,
        [title, content, context.user.id],
        false
      );
      return rows[0];
    },

    createComment: async (_, { noteId, content }, context) => {
      if (!context.user) {
        throw new GraphQLError('Unauthorized', { extensions: { code: 'UNAUTHENTICATED' } });
      }
      await assertNoteAccess(context.user.id, noteId, 'editor');
      const { rows } = await queryDB(
        `INSERT INTO comments (note_id, author_id, content)
         VALUES ($1::uuid, $2::uuid, $3)
         RETURNING id::text, note_id::text, author_id::text, content,
                   to_char(created_at, 'YYYY-MM-DD"T"HH24:MI:SS"Z"') as created_at`,
        [noteId, context.user.id, content],
        false
      );
      return rows[0];
    },

    deleteComment: async (_, { id }, context) => {
      if (!context.user) {
        throw new GraphQLError('Unauthorized', { extensions: { code: 'UNAUTHENTICATED' } });
      }
      const { rows } = await queryDB(
        `SELECT note_id::text FROM comments WHERE id = $1::uuid`,
        [id]
      );
      if (rows.length === 0) return false;
      await assertNoteAccess(context.user.id, rows[0].note_id);
      const { rowCount } = await queryDB(
        `DELETE FROM comments WHERE id = $1::uuid AND author_id = $2::uuid`,
        [id, context.user.id],
        false
      );
      return rowCount > 0;
    },
  },

  Note: {
    ownerId: (parent) => parent.owner_id || parent.ownerId,
    createdAt: (parent) => parent.created_at || parent.createdAt,
    author: (parent, _, context) => {
      const ownerId = parent.owner_id || parent.ownerId;
      if (!ownerId) return null;
      return context.loaders.userLoader.load(ownerId);
    },
    comments: (parent, _, context) => {
      return context.loaders.commentsLoader.load(parent.id);
    },
  },

  User: {
    email: (parent, _, context) => {
      if (context.user && context.user.id === (parent.id || parent.user_id)) {
        return parent.email || null;
      }
      return null;
    },
  },

  Comment: {
    noteId: (parent) => parent.note_id || parent.noteId,
    createdAt: (parent) => parent.created_at || parent.createdAt,
    author: (parent, _, context) => {
      const authorId = parent.author_id || parent.authorId;
      if (!authorId) return null;
      return context.loaders.userLoader.load(authorId);
    },
  },
};

async function startServer() {
  const app = express();
  const server = new ApolloServer({
    typeDefs,
    resolvers,
  });

  await server.start();

  app.use(cors());
  app.use(express.json());

  // Tracing and metrics middleware
  app.use((req, res, next) => {
    const startTime = Date.now();
    const tp = req.headers['traceparent'] || req.headers['x-trace-id'];
    let traceId = crypto.randomBytes(16).toString('hex');
    let spanId = crypto.randomBytes(8).toString('hex');
    if (tp && typeof tp === 'string' && tp.startsWith('00-')) {
      const parts = tp.split('-');
      if (parts.length >= 3) {
        traceId = parts[1];
      }
    }
    req.traceId = traceId;
    req.spanId = spanId;

    res.on('finish', () => {
      const durationSec = (Date.now() - startTime) / 1000;
      const statusStr = String(res.statusCode);
      httpRequestsTotal.inc({ service: 'graphql-service', method: req.method, status: statusStr });
      httpRequestDurationSeconds.observe({ service: 'graphql-service', method: req.method }, durationSec);
      logJson('INFO', `${req.method} ${req.path} HTTP ${res.statusCode}`, traceId, spanId);
    });

    next();
  });

  // Prometheus Metrics endpoint
  app.get('/metrics', async (req, res) => {
    try {
      res.set('Content-Type', promClient.register.contentType);
      res.end(await promClient.register.metrics());
    } catch (err) {
      res.status(500).end(err);
    }
  });

  // Health check endpoint with pool metrics and circuit breaker status
  app.get('/health', (req, res) => {
    res.json({
      status: dbCircuitBreaker.state === 'OPEN' ? 'degraded' : 'ok',
      service: 'graphql',
      pool: {
        total: dbPool.totalCount,
        idle: dbPool.idleCount,
        waiting: dbPool.waitingCount,
      },
      circuitState: dbCircuitBreaker.state,
    });
  });

  app.use(
    '/graphql',
    expressMiddleware(server, {
      context: async ({ req }) => {
        let user = null;
        let token = null;

        const authHeader = req.headers.authorization;
        if (authHeader && authHeader.startsWith('Bearer ')) {
          token = authHeader.substring(7);
        }

        if (token) {
          try {
            const decoded = jwt.verify(token, JWT_SECRET, {
              algorithms: ['HS256'],
              issuer: 'notenest'
            });
            user = { id: decoded.user_id };
          } catch (err) {
            logJson('WARN', `JWT verification failed: ${err.message}`, req.traceId, req.spanId);
          }
        }

        return {
          user,
          loaders: createLoaders(),
          traceId: req.traceId,
          spanId: req.spanId,
        };
      },
    })
  );

  app.listen(PORT, () => {
    logJson('INFO', `GraphQL service running on port ${PORT}`);

    const CONSUL_HOST = process.env.CONSUL_HOST || 'consul';
    const CONSUL_PORT = process.env.CONSUL_PORT || '8500';
    const CONSUL_URL = `http://${CONSUL_HOST}:${CONSUL_PORT}`;
    const hostname = require('os').hostname();
    const serviceId = `graphql-service-${hostname}-${PORT}`;

    const registerWithConsul = async () => {
      try {
        await fetch(`${CONSUL_URL}/v1/agent/service/register`, {
          method: 'PUT',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            ID: serviceId,
            Name: 'graphql-service',
            Tags: ['graphql', 'v0.19'],
            Address: hostname,
            Port: parseInt(PORT, 10),
            Check: {
              CheckID: `service:${serviceId}`,
              Name: 'GraphQL Service TTL Health Check',
              TTL: '10s',
              DeregisterCriticalServiceAfter: '30s',
            },
          }),
        });
        logJson('INFO', `[Consul] Registered graphql-service (ID: ${serviceId})`);
      } catch (err) {
        logJson('WARN', `[Consul] Registration error: ${err.message}`);
      }
    };

    const sendConsulHeartbeat = async () => {
      try {
        await fetch(`${CONSUL_URL}/v1/agent/check/pass/service:${serviceId}`, { method: 'PUT' });
      } catch (err) {
        // ignore
      }
    };

    const deregisterConsul = async () => {
      try {
        await fetch(`${CONSUL_URL}/v1/agent/service/deregister/${serviceId}`, { method: 'PUT' });
        logJson('INFO', `[Consul] Deregistered graphql-service (ID: ${serviceId})`);
      } catch (err) {
        // ignore
      }
    };

    registerWithConsul().then(() => sendConsulHeartbeat());
    const heartbeatInterval = setInterval(sendConsulHeartbeat, 4000);

    const shutdown = async () => {
      clearInterval(heartbeatInterval);
      await deregisterConsul();
      process.exit(0);
    };

    process.on('SIGINT', shutdown);
    process.on('SIGTERM', shutdown);
  });
}

startServer().catch((err) => {
  console.error('Failed to start GraphQL server:', err);
  process.exit(1);
});
