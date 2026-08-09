# RUNBOOK-003: Pod CrashLoopBackOff & Resource Starvation

## Symptoms & Alerts
- Alert: `KubernetesPodCrashLooping` (Pod restarting > 5 times in 10 minutes).
- Alert: `KubernetesMemoryOOMKilled` (Container terminated due to OOM limit).

## Initial Triage
1. List failing pods and restart counts:
   `kubectl get pods --field-selector=status.phase!=Running`
2. Inspect pod event history and exit code:
   `kubectl describe pod <pod-name>`
3. Fetch last terminated container logs:
   `kubectl logs <pod-name> --previous`

## Mitigation Steps
1. OOMKilled (Exit Code 137):
   - Increase memory request and limit in Helm values (`deployments/helm/notenest/values.yaml`):
     ```yaml
     resources:
       limits:
         memory: 512Mi
       requests:
         memory: 256Mi
     ```
   - Upgrade deployment: `helm upgrade notenest deployments/helm/notenest`
2. Unhandled Exception or Configuration Error:
   - Check ConfigMap and Secret mounts: `kubectl get configmap`, `kubectl get secrets`.
   - Verify environment variables (e.g. database host, auth secrets) are correctly populated.
3. Health Probe Failure:
   - Verify `/health` endpoint status manually from inside cluster or bastion container:
     `curl http://<pod-ip>:8080/health`
