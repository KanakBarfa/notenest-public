import http from 'k6/http';
import { check, sleep } from 'k6';

export const options = {
  stages: [
    { duration: '30s', target: 50 },
    { duration: '1m', target: 200 },
    { duration: '30s', target: 0 },
  ],
  thresholds: {
    http_req_duration: ['p(95)<500', 'p(99)<1000'],
    http_req_failed: ['rate<0.01'],
  },
};

const BASE_URL = __ENV.BASE_URL || 'http://localhost:8000';

export default function () {
  const payload = JSON.stringify({
    username: `user_${__VU}_${__ITER}`,
    email: `user_${__VU}_${__ITER}@example.com`,
    password: 'Password123!',
  });

  const params = {
    headers: {
      'Content-Type': 'application/json',
    },
  };

  const registerRes = http.post(`${BASE_URL}/api/auth/register`, payload, params);
  check(registerRes, {
    'register or login succeeded': (r) => r.status === 200 || r.status === 201 || r.status === 409,
  });

  const loginRes = http.post(`${BASE_URL}/api/auth/login`, JSON.stringify({
    username: `user_${__VU}_${__ITER}`,
    password: 'Password123!',
  }), params);

  if (loginRes.status === 200) {
    const body = JSON.parse(loginRes.body);
    const token = body.token || (body.data && body.data.token);

    if (token) {
      const authParams = {
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${token}`,
        },
      };

      const notesRes = http.get(`${BASE_URL}/api/notes`, authParams);
      check(notesRes, { 'get notes status 200': (r) => r.status === 200 });

      const createNoteRes = http.post(`${BASE_URL}/api/notes`, JSON.stringify({
        title: `Load Test Note ${__VU}-${__ITER}`,
        content: 'This note was created during a k6 automated load testing scenario.',
      }), authParams);
      check(createNoteRes, { 'create note status 200/201': (r) => r.status === 200 || r.status === 201 });
    }
  }

  sleep(1);
}
