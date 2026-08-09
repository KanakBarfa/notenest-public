import http from 'k6/http';
import { check, sleep } from 'k6';

export const options = {
  stages: [
    { duration: '15s', target: 30 },
    { duration: '45s', target: 100 },
    { duration: '15s', target: 0 },
  ],
  thresholds: {
    http_req_duration: ['p(99)<1500'],
    http_req_failed: ['rate<0.05'],
  },
};

const BASE_URL = __ENV.BASE_URL || 'http://localhost:8000';

export default function () {
  const params = {
    headers: {
      'Content-Type': 'application/json',
    },
  };

  const healthRes = http.get(`${BASE_URL}/health`, params);
  check(healthRes, { 'health check ok': (r) => r.status === 200 });

  const notesRes = http.get(`${BASE_URL}/api/notes`, params);
  check(notesRes, { 'notes query handled': (r) => r.status === 200 || r.status === 401 || r.status === 503 });

  sleep(0.5);
}
