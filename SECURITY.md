### Operating the Xahau server securely

For more details on operating the Xahau server securely, please visit https://docs.xahau.network/infrastructure/building-xahau.


# Security Policy

## Supported Versions

Software constantly evolves. In order to focus resources, we only generally only accept vulnerability reports that affect recent and current versions of the software. We always accept reports for issues present in the **release**, **candidate** or **dev** branches, and with proposed, [open pull requests](https://github.com/xahau/xahaud/pulls).

# Responsible Disclosure

## Responsible disclosure policy

At [Xahau](https://xahau.network) we believe that the security of our systems is extremely important.

Despite our concern for the security of our systems during product development and maintenance, there's always the possibility of someone finding something we need to improve / update / change / fix / ...

We appreciate you notifying us if you found a weak point in one of our systems as soon as possible so that we can take measures immediately to protect our customers and their data.

## How to report

If you believe you found a security issue in one of our systems, please notify us as soon as possible by [send an email to bugs@xahau.network](mailto:bugs@xahau.network).

## Rules

This responsible disclosure policy is not an open invitation to actively scan our network and applications for vulnerabilities. Our continuous monitoring will likely detect your scan and these will be investigated.

### We ask you to:

- Not share information about the security issue with others until the problem is resolved and to immediately delete any confidential data acquired
- Not further abuse the problem, for example, by downloading more data than is necessary in order to demonstrate the leak or to view, delete or amend the data of third parties
- Provide detailed information in order for us to reproduce, validate and resolve the problem as quickly as possible. Include your test data, timestamps and URL(s) of the system(s) involved
- Leave your contact details (e-mail address and/or phone number) so that we may contact you about the progress of the solution. We do accept anonymous reports
- Do not use attacks on physical security, social engineering, distributed denial of service, spam or applications of third parties

## Responsible Disclosure procedure(s)

### When you report a security issue, we will act according to the following:

- You will receive a confirmation of receipt from us within 4 working days after the report was made
- You will receive a response with the assessment of the security issue and an expected date of resolution within 4 working days after the confirmation of receipt was sent
- We will take no legal steps against you in relation to the report if you have kept to the conditions as set out above
- We will handle your report confidentially and we will not share your details with third parties without your permission, unless that is necessary in order to fulfil a legal obligation

### This responsible disclosure scheme is not intended for:

- Complaints
- Website unavailable reports
- Phishing reports
- Fraud reports

For these complaints or reports, please [contact our support team](mailto:bugs@xahau.network).

## Bug bounty program

[Xahau](https://xahau.network) encourages the reporting of security issues or vulnerabilities. We may make an appropriate reward for confidential disclosure of any design or implementation issue that could be used to compromise the confidentiality or integrity of our users' data that was not yet known to us. We decide whether the report is eligible and the amount of the reward.

## Exclusions

### The following type of security problems are excluded

1. **In scope**. Only bugs in software under the scope of the program qualify. Currently, that means `xahaud` and `xahau-lib`.
2. **Relevant**. A security issue, posing a danger to user funds, privacy or the operation of the Xahau Ledger.
3. **Original and previously unknown**. Bugs that are already known and discussed in public do not qualify. Previously reported bugs, even if publicly unknown, are not eligible.
4. **Specific**. We welcome general security advice or recommendations, but we cannot pay bounties for that.
5. **Fixable**. There has to be something we can do to permanently fix the problem. Note that bugs in other people’s software may still qualify in some cases. For example, if you find a bug in a library that we use which can compromise the security of software that is in scope and we can get it fixed, you may qualify for a bounty.
6. **Unused**. If you use the exploit to attack the Xahau Ledger, you do not qualify for a bounty. If you report a vulnerability used in an ongoing or past attack and there is specific, concrete evidence that suggests you are the attacker we reserve the right not to pay a bounty.

Please note: Reports that are lacking any proof (such as screenshots or other data), detailed information or details on how to reproduce any unexpected result will be investigated but will not be eligible for any reward.

This policy is based on the National Cyber Security Centre’s Responsible Disclosure Guidelines and an [example by Floor Terra](https://responsibledisclosure.nl).
