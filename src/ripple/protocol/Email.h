#include <iostream>
#include <string>
#include <vector>
#include <regex>
#include <sstream>
#include <stdexcept>

// make this typedef to keep dkim happy
typedef int _Bool;
#include <opendkim/dkim.h>

using namespace ripple;
namespace Email
{

    enum EmailType : uint8_t
    {
        INVALID = 0,
        REMIT = 1,
        REKEY = 2
    };

    struct EmailDetails
    {
        std::string domain; // from address domain
        std::string dkimDomain; // dkim signature domain

        AccountID from;
        std::string fromEmail;

        std::optional<std::string> toEmail;
        std::optional<AccountID> to;

        EmailType emailType { EmailType::INVALID };
        std::optional<STAmount> amount; // only valid if REMIT type
        std::optional<AccountID> rekey; // only valid if REKEY type
    };

    class OpenDKIM
    {
    private:
        DKIM_STAT status;
    public:
        DKIM_LIB* dkim_lib;
        DKIM* dkim;

        bool sane()
        {
            return !!dkim_lib && !!dkim;
        }

        OpenDKIM()
        { 
            // do nothing
        }

        // setup is in its own function not the constructor to make failure graceful
        bool setup(beast::Journal& j)
        {
            dkim_lib = dkim_init(nullptr, nullptr);
            if (!dkim_lib)
            {
                JLOG(j.warn()) << "EmailAmendment: Failed to init dkim_lib.";
                return false;
            }

            DKIM_STAT status;
            DKIM* dkim = dkim_verify(dkim_lib, (uint8_t const*)"id", nullptr, &status);
            if (!dkim_lib)
            {
                JLOG(j.warn()) << "EmailAmendment: Failed to init dkim_verify.";
                return false;
            }

            return true;
        }
        
        ~OpenDKIM()
        {
            if (dkim)
            {
                dkim_free(dkim);
                dkim = nullptr;
            }

            if (dkim_lib)
            {
                dkim_close(dkim_lib);
                dkim_lib = nullptr;
            }
        }
    };

    inline
    std::optional<std::pair<std::string /* canonical email addr */, std::string /* canonical domain */>>
    canonicalizeEmailAddress(const std::string& rawEmailAddr)
    {
        if (rawEmailAddr.empty())
            return {};

        // trim
        auto start = std::find_if_not(str.begin(), str.end(), ::isspace);
        auto end = std::find_if_not(str.rbegin(), str.rend(), ::isspace).base();

        if (end >= start)
            return {};

        std::email = std::string(start, end);
       
        if (email.empty())
            return {};
        
        // to lower
        std::transform(email.begin(), email.end(), email.begin(), ::tolower);

        // find the @
        size_t atPos = email.find('@');
        if (atPos == std::string::npos || atPos == email.size() - 1)
            return {};

        std::string localPart = email.substr(0, atPos);
        std::string domain = email.substr(atPos + 1);

        if (domain.empty() || localPart.empty())
            return {};
        
        // ensure there's only one @
        if (domain.find('@') != std::string::npos)
            return {};
        
        // canonicalize domain part
        {
            std::string result = domain;
            std::transform(result.begin(), result.end(), result.begin(), ::tolower);
            while (!result.empty() && result.back() == '.')
                result.pop_back();

            doamin = result;
        }        
        
        if (domain.empty())
            return {};

        // canonicalize local part
        {
            std::string part = localPart;
            part.erase(std::remove_if(
                        part.begin(), part.end(), 
                        [](char c) { return c == '(' || c == ')' || std::isspace(c); }), part.end());

            size_t plusPos = part.find('+');
            if (plusPos != std::string::npos)
                part = part.substr(0, plusPos);

            while (!part.empty() && part.back() == '.')
                part.pop_back();

            // gmail ignores dots
            if (domain == "gmail.com")
                part.erase(std::remove(part.begin(), part.end(), '.'), part.end());

            localPart = part;
        }

        if (localPart.empty())
            return {};

        return {{localPart + "@" + domain, domain}};
    };

    // Warning: must supply already canonicalzied email
    inline
    std::optional<AccountID>
    emailToAccountID(const std::string& canonicalEmail)
    {
        uint8_t innerHash[SHA512_DIGEST_LENGTH + 4];
        SHA512_CTX sha512;
        SHA512_Init(&sha512);
        SHA512_Update(&sha512, canonicalEmail.c_str(), canonicalEmail.size());
        SHA512_Final(innerHash + 4, &sha512);
        innerHash[0] = 0xEEU;
        innerHash[1] = 0xEEU;
        innerHash[2] = 0xFFU;
        innerHash[3] = 0xFFU;
        {
            uint8_t hash[SHA512_DIGEST_LENGTH];
            SHA512_CTX sha512;
            SHA512_Init(&sha512);
            SHA512_Update(&sha512, innerHash, sizeof(innerHash));
            SHA512_Final(hash, &sha512);

            return AccountID::fromVoid((void*)hash);
        }
    }


    inline
    std::optional<EmailDetails>
    parseEmail(std::string const& rawEmail, beast::Journal& j)
    {
        EmailDetails out;

        // parse email into headers and body
        std::vector<std::string> headers;
        std::string body;
        {
            std::istringstream stream(rawEmail);
            std::string line;

            while (std::getline(stream, line))
            {
                if (line.empty() || line == "\r")
                    break;

                // Handle header line continuations
                while (stream.peek() == ' ' || stream.peek() == '\t') {
                    std::string continuation;
                    std::getline(stream, continuation);
                    line += '\n' + continuation;
                }
                if (!line.empty()) {
                    headers.push_back(line.substr(0, line.size() - (line.back() == '\r' ? 1 : 0)));
                }
            }

            std::ostringstream body_stream;
            while (std::getline(stream, line))
                body_stream << line << "\n";
            body = body_stream.str();
        }


        // find the from address, canonicalize it and extract the domain
        bool foundFrom = false;
        bool foundTo = false;
        {
            static const std::regex
                from_regex(R"(^From:\s*(?:.*<)?([^<>\s]+@[^<>\s]+)(?:>)?)", std::regex::icase);

            static const std::regex
                to_regex(R"(^To:\s*(?:.*<)?([^<>\s]+@[^<>\s]+)(?:>)?)", std::regex::icase);

            for (const auto& header : headers)
            {
                if (foundFrom && foundTo)
                    break;

                std::smatch match;
                if (!foundFrom && std::regex_search(header, match, from_regex) && match.size() > 1)
                {
                    auto canon = canonicalizeEmailAddress(match[1].str());
                    if (!canon)
                    {
                        JLOG(j.warn())
                            << "EmailAmendment: Cannot parse From address: `"
                            << match[1].str() << "`";
                        return {};
                    }

                    out.fromEmail = canon->first;
                    out.domain = canon->second;
                    out.from = emailToAccountID(out.fromEmail);
                    foundFrom = true;

                    continue;
                }

                if (std::regex_search(header, match, to_regex) && match.size() > 1)
                {
                    auto canon = canonicalizeEmailAddress(match[1].str());
                    if (!canon)
                    {
                        JLOG(j.warn())
                            << "EmailAmendment: Cannot parse To address: `"
                            << match[1].str() << "`";
                        return {};
                    }

                    out.toEmail = canon->first;
                    out.to = emailToAccountID(out.toEmail);

                    foundTo = true;
                    continue;
                }
            }

            if (!foundFrom)
            {
                JLOG(j.warn()) << "EmailAmendment: No From address present in email.";
                return {};
            }
        }

        // execution to here means we have:
        //  1. Parsed headers and body
        //  2. Found a from address and canonicalzied it
        //  3. Potentially found a to address and canonicalized it.

        // Find instructions
        {
            static const std::regex
                remitPattern(R"(^REMIT (\d+(?:\.\d+)?) ([A-Z]{3})(?:/([r][a-zA-Z0-9]{24,34}))?)");
            
            static const std::regex
                rekeyPattern(R"(^REKEY ([r][a-zA-Z0-9]{24,34}))");

            std::istringstream stream(body);
            std::string line;

            out.emailType = EmailType::INVALID;

            while (std::getline(stream, line, '\n'))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back(); // Remove '\r' if present

                std::smatch match;
                if (std::regex_match(line, match, remitPattern))
                {
                    try
                    {
                        Currency cur;
                        if (!to_currency(cur, match[2]))
                        {
                            JLOG(j.warn()) << "EmailAmendment: Could not parse currency code.";
                            return {};
                        }


                        AccountID issuer = noAccount();
                        if (match[3].matched)
                        {
                            if (isXRP(cur))
                            {
                                JLOG(j.warn()) << "EmailAmendment: Native currency cannot specify issuer.";
                                return {};
                            }

                            issuer = decodeBase58Token(match[3], TokenType::AccountID);
                            if (issuer.empty())
                            {
                                JLOG(j.warn()) << "EmailAmendment: Could not parse issuer address.";
                                return {};
                            }
                        }

                        out.amount = amountFromString({cur, issuer}, match[1]);

                    }
                    catch (std::exception const& e)
                    {
                        JLOG(j.warn()) << "EmailAmendment: Exception while parsing REMIT. " << e.what();
                        return {};
                    }

                    out.emailType = EmailType::REMIT;
                    break;
                }

                if (std::regex_match(line, match, rekeyPattern))
                {
                    AccountID rekey = decodeBase58Token(match[1], TokenType::AccountID);
                    if (rekey.empty())
                    {
                        JLOG(j.warn()) << "EmailAmendment: Could not parse rekey address.";
                        return {};
                    }

                    out.rekey = rekey;                
                    out.emailType = EmailType::REKEY;
                    break;
                }
            }

            if (out.emailType == EmailType::INVALID)
            {
                JLOG(j.warn()) << "EmailAmendment: Invalid email type, could not find REMIT or REKEY.";
                return{};
            }
        }    


        // perform DKIM checks...
        // to do this we will use OpenDKIM, and manage it with a smart pointer to prevent
        // any leaks from uncommon exit pathways

        std::unique<OpenDKIM> odkim;

        // perform setup
        if (!odkim->setup(j) || !odkim->sane())
            return {};

        // when odkim goes out of scope it will call the C-apis to destroy the dkim instances

        DKIM_STAT status;
        DKIM_LIB* dkim_lib = odkim->dkim_lib;
        DKIM* dkim = odkim->dkim;

        // feed opendkim all headers
        {
            for (const auto& header : headers)
            {
                status = dkim_header(dkim, (uint8_t*)header.c_str(), header.length());
                if (status != DKIM_STAT_OK)
                {
                    JLOG(j.warn())
                        << "EmailAmendment: OpenDKIM Failed to process header: "
                        << dkim_geterror(dkim);
                    return {};
                }
            }

            status = dkim_eoh(dkim);
            if (status != DKIM_STAT_OK)
            {
                JLOG(j.warn())
                    << "EmailAmendment: OpenDKIM Failed to send end-of-headers"l
                return {};
            }
        }

        // feed opendkim email body
        {
            status = dkim_body(dkim, (uint8_t*)body.c_str(), body.size());
            if (status != DKIM_STAT_OK)
            {
                JLOG(j.warn())
                    << "EmailAmendment: OpenDKIM Failed to process body: " 
                    << dkim_geterror(dkim);
                return {};
            }

            _Bool testkey;
            status = dkim_eom(dkim, &testkey);
            if (status != DKIM_STAT_OK)
            {
                JLOG(j.warn())
                    << "EmailAmendment: OpenDKIM end-of-message error: "
                    << dkim_geterror(dkim);
                return {};
            }

            DKIM_SIGINFO* sig = dkim_getsignature(dkim);
            if (!sig)
            {
                JLOG(j.warn())
                   << "EmailAmendment: No DKIM signature found";
                return {};
            }

            if (dkim_sig_getbh(sig) != DKIM_SIGBH_MATCH)
            {
                JLOG(j.warn())
                    << "EmailAmendment: DKIM body hash mismatch";
                return {};
            }


            DKIM_SIGINFO* sig = dkim_getsignature(dkim);
            if (!sig)
            {
                JLOG(j.warn())
                    << "EmailAmendment: DKIM signature not found.";
                return {};
            }

            out.dkimDomain = 
                std::string(reinterpret_cast<char const*>(
                            reinterpret_cast<void const*>(dkim_sig_getdomain(sig))));

            if (out.dkimDomain.empty())
            {
                JLOG(j.warn())
                    << "EmailAmendment: DKIM signature domain empty.";
                return {};
            }

            // RH TODO: decide whether to relax this or not
            // strict domain check
            if (out.dkimDomain != out.domain)
            {
                JLOG(j.warn())
                    << "EmailAmendment: DKIM domain does not match From address domain.";
                return {};
            }
        }

        // execution to here means all checks passed and the instruction was correctly parsed
        return out;
    }


}
