FROM python:3.11-slim

WORKDIR /app

COPY services/notification/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY protos/ /app/protos/
RUN python -m grpc_tools.protoc -I/app/protos --python_out=. --grpc_python_out=. /app/protos/note.proto

COPY services/consul_client.py ./
COPY services/obs_helper.py ./
COPY services/notification/main.py ./

EXPOSE 9103

CMD ["python", "main.py"]
