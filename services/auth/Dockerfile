FROM python:3.11-slim

WORKDIR /app

COPY services/auth/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY protos/auth.proto /app/protos/
RUN python -m grpc_tools.protoc -I/app/protos --python_out=/app --grpc_python_out=/app /app/protos/auth.proto

COPY services/consul_client.py .
COPY services/obs_helper.py .
COPY services/auth/server.py .

EXPOSE 50051 9101

CMD ["python", "server.py"]
