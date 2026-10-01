# Linux benchmark on EC2, from GitHub Actions

The workflow [`.github/workflows/benchmark.yml`](../.github/workflows/benchmark.yml)
runs the official pinned-core benchmark on a temporary EC2 instance:

```
Run workflow ─► start-runner ─► benchmark (on EC2) ─► open-pr ─► stop-runner (always)
                launch c7i,      download day, pin     PR with     terminate the
                single-use       cores, 3 configs,     results/    instance, plus a
                runner           golden check, charts  linux       tag-based sweep
```

You get an **artifact** (`linux-benchmark-<run id>`, kept 30 days) and a **pull
request** that adds `results/linux/` and `docs/linux/`. The PR body is the
results table. Nothing is committed to `main` until you merge the PR.

## One-time setup (about 10 minutes)

### 1. AWS: role, network, quota

You need the AWS CLI with admin credentials for your account (`aws configure`
or `aws sso login`). Then:

```bash
bench/aws/setup_aws.sh
```

The script is idempotent and creates only these resources:

| Resource | What it can do |
|---|---|
| OIDC identity provider `token.actions.githubusercontent.com` | lets GitHub Actions prove which repo and branch it is |
| IAM role `itch-benchmark-github` | assumable **only** by `Tharun-Maheswararao/itch_feed_handler` on `main` ([trust policy](../bench/aws/trust-policy.json)). It may launch only `c7i.2xlarge`, `c7i.4xlarge` and `c7i.metal-24xl`, only with the benchmark tag, and terminate only instances carrying that tag ([permissions](../bench/aws/permissions-policy.json)) |
| Security group `itch-benchmark-runner` | no inbound rules; outbound only |

It also picks a default subnet in an availability zone that offers
`c7i.2xlarge`, and checks your on-demand vCPU quota. New accounts often allow
fewer than the 8 vCPUs a `c7i.2xlarge` needs. If the script warns, request the
increase it prints; approval usually takes minutes to a day.

At the end it prints three secrets and the `gh secret set` commands to store
them:

| Secret | Example |
|---|---|
| `AWS_ROLE_ARN` | `arn:aws:iam::123456789012:role/itch-benchmark-github` |
| `AWS_SUBNET_ID` | `subnet-0abc…` |
| `AWS_SECURITY_GROUP_ID` | `sg-0abc…` |

No AWS keys are stored anywhere: each run gets short-lived credentials through
OIDC.

### 2. GitHub: a personal access token

The instance registers itself as a runner, and the PR is opened with this token
so that CI runs on it (PRs opened with the default `GITHUB_TOKEN` do not
trigger other workflows).

1. Go to GitHub → Settings → Developer settings → **Fine-grained tokens** → Generate new token.
2. Under **Repository access**, choose *Only select repositories*: `itch_feed_handler`.
3. Under **Repository permissions**:
   * Administration: **Read and write** (register and remove the runner)
   * Contents: **Read and write** (push the results branch)
   * Pull requests: **Read and write** (open the PR)
4. Set an expiry (90 days is sensible), generate the token, and store it:

```bash
gh secret set GH_RUNNER_PAT --repo Tharun-Maheswararao/itch_feed_handler
```

(`gh` prompts for the value, so it never lands in your shell history.)

## Running it

GitHub → **Actions** → **Linux benchmark** → **Run workflow** (on `main`):

| Input | Default | Notes |
|---|---|---|
| `instance_type` | `c7i.2xlarge` | 4 physical cores, 16 GB. `c7i.metal-24xl` has no hypervisor or noisy neighbours, for the cleanest tails, at about $4/h |
| `rate` | `2000000` | paced replay rate |
| `runs` | `5` | timed runs per configuration, after one warmup |

Or from the terminal:

```bash
gh workflow run benchmark.yml --repo Tharun-Maheswararao/itch_feed_handler -f instance_type=c7i.2xlarge
```

A run takes about **50–60 minutes**: about 10 minutes of setup and download,
about 35 minutes of benchmark on the full day, and about 5 minutes for the
golden check.

**Cost:** a `c7i.2xlarge` in us-east-1 is about $0.36/h, so **under $0.50 per
run** including the 40 GB disk. GitHub-hosted minutes for the small start,
stop and PR jobs are free on public repos.

## Safety nets

* `stop-runner` runs with `if: always()`: it terminates the instance on
  success, failure or cancellation.
* It then **terminates anything tagged with this run's id**, which covers a
  start step that launched the instance but failed before reporting its id.
* The instance schedules its own power-off 3 hours after boot, in case GitHub
  never reaches the stop job. A stopped instance has no compute charge.
* `concurrency` prevents two benchmark instances at once.
* The runner is single-use (JIT) and only starts on manual dispatch, so no pull
  request from a fork can ever run code on it.

To check that nothing is left behind:

```bash
aws ec2 describe-instances --region us-east-1 --filters Name=tag:Project,Values=itch-feed-handler-benchmark Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].[InstanceId,State.Name,LaunchTime]' --output table
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Not authorized to perform sts:AssumeRoleWithWebIdentity` | The workflow must run from `main`, and `AWS_ROLE_ARN` must be the ARN printed by the setup script |
| `VcpuLimitExceeded` | Request the quota increase printed by the setup script |
| `UnauthorizedOperation` on `RunInstances` | The instance type is not in the allowed list in `permissions-policy.json`. Add it and re-run `setup_aws.sh` |
| `InsufficientInstanceCapacity` | Retry later, or re-run the setup script with `INSTANCE_TYPE=c7i.4xlarge` to choose a subnet in a different AZ |
| Runner never registers (start job times out) | Re-run with `runner-debug: true` added to the start step to see the instance console |
| Removing everything | Delete the role `itch-benchmark-github`, the security group `itch-benchmark-runner`, and (if nothing else uses it) the OIDC provider |
