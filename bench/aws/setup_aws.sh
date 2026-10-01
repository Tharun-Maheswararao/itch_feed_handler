#!/usr/bin/env bash
# One-time AWS setup for .github/workflows/benchmark.yml. Safe to re-run.
#
#   bench/aws/setup_aws.sh
#
# Run it with admin credentials for your own AWS account (aws configure / SSO).
# It creates, in us-east-1 unless REGION is set:
#   * the GitHub OIDC identity provider (if the account does not have it yet)
#   * IAM role  itch-benchmark-github  that only this repo's main branch can
#     assume, limited to launching/terminating tagged benchmark instances
#   * security group  itch-benchmark-runner  in the default VPC: no inbound
#     rules, outbound allowed (the runner only dials out to GitHub and Nasdaq)
# and prints the three values to store as GitHub repository secrets.
set -euo pipefail

REGION=${REGION:-us-east-1}
REPO=${REPO:-Tharun-Maheswararao/itch_feed_handler}
ROLE_NAME=${ROLE_NAME:-itch-benchmark-github}
SG_NAME=${SG_NAME:-itch-benchmark-runner}
INSTANCE_TYPE=${INSTANCE_TYPE:-c7i.2xlarge}
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export AWS_DEFAULT_REGION=$REGION

ACCOUNT=$(aws sts get-caller-identity --query Account --output text)
echo "== account $ACCOUNT, region $REGION, repo $REPO"

# 1. OIDC provider for GitHub Actions
OIDC_ARN="arn:aws:iam::$ACCOUNT:oidc-provider/token.actions.githubusercontent.com"
if aws iam get-open-id-connect-provider --open-id-connect-provider-arn "$OIDC_ARN" >/dev/null 2>&1; then
  echo "== OIDC provider already exists"
else
  aws iam create-open-id-connect-provider \
    --url https://token.actions.githubusercontent.com \
    --client-id-list sts.amazonaws.com >/dev/null
  echo "== created OIDC provider"
fi

# 2. IAM role (trust: this repo, main branch only) + least-privilege policy
sed -e "s/ACCOUNT_ID/$ACCOUNT/g" -e "s#GITHUB_REPO#$REPO#g" "$DIR/trust-policy.json" >"$TMP/trust.json"
sed -e "s/ACCOUNT_ID/$ACCOUNT/g" -e "s/REGION/$REGION/g" "$DIR/permissions-policy.json" >"$TMP/perms.json"
if aws iam get-role --role-name "$ROLE_NAME" >/dev/null 2>&1; then
  aws iam update-assume-role-policy --role-name "$ROLE_NAME" --policy-document "file://$TMP/trust.json"
  echo "== updated role $ROLE_NAME"
else
  aws iam create-role --role-name "$ROLE_NAME" \
    --description "GitHub Actions: launch and terminate itch_feed_handler benchmark instances" \
    --assume-role-policy-document "file://$TMP/trust.json" >/dev/null
  echo "== created role $ROLE_NAME"
fi
aws iam put-role-policy --role-name "$ROLE_NAME" --policy-name itch-benchmark-ec2 \
  --policy-document "file://$TMP/perms.json"
ROLE_ARN=$(aws iam get-role --role-name "$ROLE_NAME" --query Role.Arn --output text)

# 3. Network: default VPC, a subnet in an AZ that offers the instance type
VPC=$(aws ec2 describe-vpcs --filters Name=isDefault,Values=true --query 'Vpcs[0].VpcId' --output text)
if [[ "$VPC" == "None" || -z "$VPC" ]]; then
  echo "error: no default VPC in $REGION. Create one with: aws ec2 create-default-vpc" >&2
  exit 1
fi
AZS=$(aws ec2 describe-instance-type-offerings --location-type availability-zone \
  --filters "Name=instance-type,Values=$INSTANCE_TYPE" --query 'InstanceTypeOfferings[].Location' --output text)
SUBNET=""
for az in $AZS; do
  SUBNET=$(aws ec2 describe-subnets --filters "Name=vpc-id,Values=$VPC" "Name=availability-zone,Values=$az" \
    "Name=default-for-az,Values=true" --query 'Subnets[0].SubnetId' --output text)
  [[ "$SUBNET" != "None" && -n "$SUBNET" ]] && break
  SUBNET=""
done
if [[ -z "$SUBNET" ]]; then
  echo "error: no default subnet in an AZ offering $INSTANCE_TYPE" >&2
  exit 1
fi
echo "== subnet $SUBNET (VPC $VPC)"

SG=$(aws ec2 describe-security-groups --filters "Name=vpc-id,Values=$VPC" "Name=group-name,Values=$SG_NAME" \
  --query 'SecurityGroups[0].GroupId' --output text)
if [[ "$SG" == "None" || -z "$SG" ]]; then
  SG=$(aws ec2 create-security-group --group-name "$SG_NAME" --vpc-id "$VPC" \
    --description "itch benchmark runner: outbound only" --query GroupId --output text)
  echo "== created security group $SG (no inbound rules)"
else
  echo "== security group $SG already exists"
fi

# 4. Capacity check: new accounts often have a small on-demand vCPU quota
NEED=$(aws ec2 describe-instance-types --instance-types "$INSTANCE_TYPE" \
  --query 'InstanceTypes[0].VCpuInfo.DefaultVCpus' --output text)
QUOTA=$(aws service-quotas get-service-quota --service-code ec2 --quota-code L-1216C47A \
  --query 'Quota.Value' --output text 2>/dev/null || echo unknown)
echo "== on-demand standard vCPU quota: $QUOTA ($INSTANCE_TYPE needs $NEED)"
if [[ "$QUOTA" != "unknown" ]] && (( ${QUOTA%.*} < NEED )); then
  echo "   WARNING: quota too small. Request an increase to at least $NEED:"
  echo "   aws service-quotas request-service-quota-increase --service-code ec2 --quota-code L-1216C47A --desired-value $NEED"
fi

cat <<EOF

Done. Add these repository secrets (Settings -> Secrets and variables -> Actions),
or run the gh commands below:

  AWS_ROLE_ARN           $ROLE_ARN
  AWS_SUBNET_ID          $SUBNET
  AWS_SECURITY_GROUP_ID  $SG

  gh secret set AWS_ROLE_ARN          --repo $REPO --body "$ROLE_ARN"
  gh secret set AWS_SUBNET_ID         --repo $REPO --body "$SUBNET"
  gh secret set AWS_SECURITY_GROUP_ID --repo $REPO --body "$SG"

Plus GH_RUNNER_PAT: a fine-grained personal access token (see docs/AWS_BENCHMARK.md).
EOF
