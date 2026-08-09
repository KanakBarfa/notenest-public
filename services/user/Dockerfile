FROM python:3.11-slim

WORKDIR /app

COPY services/user/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY protos/user.proto /app/protos/
RUN python -m grpc_tools.protoc -I/app/protos --python_out=/app --grpc_python_out=/app /app/protos/user.proto

COPY services/consul_client.py .
COPY services/obs_helper.py .
COPY services/user/server.py .

EXPOSE 50052 9102

CMD ["python", "server.py"]
