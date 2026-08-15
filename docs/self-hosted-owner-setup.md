# Self-Hosted Owner Setup

This is the primary ButterFi ownership model: the buyer owns the AWS account,
the Sidewalk registration, and the flashed device.

## Target Outcome

After setup, the owner has:

- a ButterFi XIAO device flashed with a prebuilt UF2 and a Sidewalk credential
- a ButterFi CloudFormation stack running in their own AWS account
- a Sidewalk wireless device registered against the destination created by that stack
- a browser provisioning flow that can be hosted on GitHub Pages or any other HTTPS static host

## What CloudFormation Does

Deploying [template.yaml](../template.yaml) creates the backend path: the
Sidewalk destination, IoT rules, Lambdas, SQS queue, DynamoDB table, logs, and
alarms.

It does not create per-device Sidewalk credentials for the buyer.

The important stack output for device onboarding today is:

- `SidewalkDestinationName`

The other outputs are operational or debugging outputs for the stack itself.

## Buyer Workflow

1. Buy a XIAO module or a ButterFi kit.
2. Create the ButterFi stack in the buyer's AWS account.
3. Copy the `SidewalkDestinationName` output from the deployed stack.
4. In AWS IoT Wireless, create or register the buyer's Sidewalk wireless device using that destination.
5. Download the combined AWS `certificate.json` for that device. If you only have CLI exports, also save the JSON responses from `get-wireless-device` and `get-device-profile`.
6. Open [web/provision.html](../web/provision.html) from GitHub Pages, another HTTPS static host, or `http://localhost`.
7. Upload a prebuilt ButterFi UF2 and either the raw `certificate.json` or, for CLI-only exports, a generated `.hex` or `.bin` credential asset from [scripts/build-sidewalk-credential.py](../scripts/build-sidewalk-credential.py), then flash the device.
8. Reconnect over the ButterFi runtime serial port and save `school_id`, `device_name`, and `content_pkg` into NVS.

## Create Stack Entry Point

If you publish [template.yaml](../template.yaml) at a stable HTTPS URL, you can
expose an AWS CloudFormation quick-create link from your docs or website.

Template:

```text
https://console.aws.amazon.com/cloudformation/home?region=us-east-1#/stacks/quickcreate?templateURL=<urlencoded-template-url>&stackName=butterfi
```

Notes:

- the template URL must be reachable by AWS over HTTPS
- for CloudFormation quick-create, the template URL itself must point to an S3 object
- the deploy still needs `CAPABILITY_NAMED_IAM` acknowledgment in the console
- GitHub Pages or another HTTPS site can host the helper page, but not the CloudFormation template target for quick-create

CLI deploy is also supported:

```bash
aws cloudformation deploy \
  --template-file template.yaml \
  --stack-name butterfi \
  --capabilities CAPABILITY_NAMED_IAM \
  --region us-east-1
```

## What The Provisioning Page Actually Uses

Today the provisioning page consumes:

- a prebuilt ButterFi UF2
- a raw AWS `certificate.json` or a Sidewalk credential asset in `.hex` or `.bin` form
- `school_id`
- `device_name`
- `content_pkg`

It does not currently consume raw CloudFormation outputs directly.

The current role of the stack output is upstream: `SidewalkDestinationName` is
used when the buyer creates the Sidewalk wireless device in AWS IoT Wireless.
The resulting AWS export is then either uploaded directly to the browser when
it is the combined `certificate.json`, or converted into a credential asset for
CLI-only export paths.

## Optional Offline Conversion

If the buyer downloaded the combined AWS `certificate.json`,
[web/provision.html](../web/provision.html) can ingest that file directly.

Offline conversion is still useful when:

- the buyer only has `get-wireless-device` and `get-device-profile` JSON exports
- the buyer wants reusable `.hex` or `.bin` assets for batch packaging
- the buyer wants a CLI-driven provisioning workflow

If the buyer downloaded a combined `certificate.json` from AWS:

```bash
python3 scripts/build-sidewalk-credential.py \
  --certificate-json provisioning/aws/device/certificate.json \
  --basename my-device \
  --output-dir provisioning/credentials
```

If the buyer exported `get-wireless-device` and `get-device-profile` JSON
instead:

```bash
python3 scripts/build-sidewalk-credential.py \
  --wireless-device-json provisioning/aws/device/wireless_device.json \
  --device-profile-json provisioning/aws/device_profile.json \
  --basename my-device \
  --output-dir provisioning/credentials
```

Either command produces `.bin` and `.hex` files that can be uploaded in
[web/provision.html](../web/provision.html) or embedded into batch packages.

## Gap Between Current UX And The Ideal UX

If you want the exact flow of "create stack, bring one thing to the browser,
flash", the remaining gap is between CloudFormation and the browser page.

The current repo still requires the buyer to:

1. leave CloudFormation and visit AWS IoT Wireless
2. create or register the Sidewalk device there
3. download the Sidewalk credential export there

If the buyer only has CLI exports rather than the combined `certificate.json`,
there is still one extra conversion step via
[scripts/build-sidewalk-credential.py](../scripts/build-sidewalk-credential.py).
The repo still does not eliminate the AWS IoT Wireless console or API detour.

## Relationship To Batch Provisioning

The batch flow in [docs/xiao-batch-provisioning.md](./xiao-batch-provisioning.md)
is optional. It is useful for manufacturing, classroom rollout, or preparing
many devices at once, but it is not the primary self-hosted owner setup path.