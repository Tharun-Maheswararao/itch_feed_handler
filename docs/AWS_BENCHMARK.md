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
| `mode` | `benchmark` | `diagnose` runs [`bench/diagnose.sh`](../bench/diagnose.sh) instead: pipeline-overhead experiments, about 35 min, artifact `linux-diagnose-<run id>` plus the run summary, no PR ; `publish` runs [`bench/publish_overhead.sh`](../bench/publish_overhead.sh): the Version 1.1 publisher off vs on, pinned, about 15 min, artifact `linux-publish-<run id>`, no PR; `net` and `net2`: Version 2, see below |
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

### Version 2 modes: `net` (one host) and `net2` (two hosts)

`mode=net` runs [`bench/net_benchmark.sh`](../bench/net_benchmark.sh) on one
instance: the publisher and the receiver in two network namespaces joined by
a veth pair, lines A and B on two multicast groups, each receive path
(kernel sockets, AF_XDP) as one seeded-loss verification run plus `runs`
clean latency runs and one full-speed run, and Version 1's file replay at
the same rate for reference. Every run is checked against
`results/v2/golden_hashes_12302019.txt`. About 50 minutes on a
`c7i.2xlarge` (under $0.50); a PR adds `results/v2/net/<instance>/`.

`mode=net2` starts a **second instance**: the first publishes, the second
receives over the VPC. A VPC carries no multicast without a Transit Gateway,
so the lines are unicast to two ports on the receiver
([`bench/net_two_host.sh`](../bench/net_two_host.sh)). The same run plan as
`net`; AF_XDP is attempted on the ENA interface (the script sets it to one
channel so every feed packet reaches the one queue the socket binds, and
restores it) and skipped, not failed, if the NIC refuses. A PR adds
`results/v2/net2/<instance>/`. About 60 minutes; two `c7i.2xlarge` cost
under $1, two `c7i.metal-24xl` about $9.

**One-time change for `net2`:** the two instances must reach each other.
Allow all traffic from the security group to itself (the instances share it;
nothing outside the group gains access):

```bash
aws ec2 authorize-security-group-ingress --region us-east-1 \
  --group-id <AWS_SECURITY_GROUP_ID> --protocol -1 --source-group <AWS_SECURITY_GROUP_ID>
```

Without it the publisher job waits for the receiver's ready port until its
150-minute timeout. Neither mode needs new IAM permissions: the role already
launches, describes and terminates tagged instances. EC2's ENA has no
hardware receive timestamps, so the rows say "kernel software" or
"user-space poll", never "NIC". DPDK on EC2 (a second interface bound to
`vfio-pci`) is not automated; the DPDK path is verified in CI only.

```bash
gh workflow run benchmark.yml --repo Tharun-Maheswararao/itch_feed_handler -f mode=net -f instance_type=c7i.2xlarge -f runs=3
```

```bash
gh workflow run benchmark.yml --repo Tharun-Maheswararao/itch_feed_handler -f mode=net2 -f instance_type=c7i.2xlarge -f runs=3
```

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
| `net` or `net2` step stopped by its 150-minute limit | Some run never finished. Every run's logs are in the `linux-net-<run id>` (or `linux-net2-…`) artifact, uploaded even after a timeout: `logs/<run>/receiver.txt` gives the stop reason. A run is also capped at 25 minutes (`RUN_TIMEOUT`) |
| `Not authorized to perform sts:AssumeRoleWithWebIdentity` | The OIDC subject in the role's trust policy must match what GitHub sends. Check with `gh api repos/OWNER/REPO/actions/oidc/customization/sub`: repos with `use_immutable_subject` send `repo:OWNER@ID/REPO@ID:ref:refs/heads/main`. Re-running `setup_aws.sh` writes the right one. The workflow must also run from `main`, and `AWS_ROLE_ARN` must be the ARN the script printed |
| `VcpuLimitExceeded` | Request the quota increase printed by the setup script |
| `UnauthorizedOperation` on `RunInstances` | The instance type is not in the allowed list in `permissions-policy.json`. Add it and re-run `setup_aws.sh` |
| `InsufficientInstanceCapacity` | Retry later, or re-run the setup script with `INSTANCE_TYPE=c7i.4xlarge` to choose a subnet in a different AZ |
| Runner never registers (start job times out) | Re-run with `runner-debug: true` added to the start step to see the instance console |
| Removing everything | Delete the role `itch-benchmark-github`, the security group `itch-benchmark-runner`, and (if nothing else uses it) the OIDC provider |
