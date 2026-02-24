#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -e

echo "START INSIDE CONTAINER - FULL"

echo "-- BUILD CORES:       $3"
echo "-- GITHUB_REPOSITORY: $1"
echo "-- GITHUB_SHA:        $2"
echo "-- GITHUB_RUN_NUMBER: $4"

umask 0000;

####

cd /io;
mkdir -p src/certs;
curl --silent -k https://raw.githubusercontent.com/RichardAH/rippled-release-builder/main/ca-bundle/certbundle.h -o src/certs/certbundle.h;
if [ "`grep certbundle.h src/xrpld/net/detail/RegisterSSLCerts.cpp | wc -l`" -eq "0" ]
then
    cp src/xrpld/net/detail/RegisterSSLCerts.cpp src/xrpld/net/detail/RegisterSSLCerts.cpp.old
    perl -i -pe "s/^{/{
    #ifdef EMBEDDED_CA_BUNDLE
    BIO *cbio = BIO_new_mem_buf(ca_bundle.data(), ca_bundle.size());
    X509_STORE  *cts = SSL_CTX_get_cert_store(ctx.native_handle());
    if(!cts || !cbio)
        JLOG(j.warn())
            << \"Failed to create cts\/cbio when loading embedded certs.\";
    else
    {
        X509_INFO *itmp;
        int i, count = 0, type = X509_FILETYPE_PEM;
        STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(cbio, NULL, NULL, NULL);

        if (!inf)
        {
            BIO_free(cbio);
            JLOG(j.warn())
                << \"Failed to read cbio when loading embedded certs.\";
        }
        else
        {
            for (i = 0; i < sk_X509_INFO_num(inf); i++)
            {
                itmp = sk_X509_INFO_value(inf, i);
                if (itmp->x509)
                {
                      X509_STORE_add_cert(cts, itmp->x509);
                      count++;
                }
                if (itmp->crl)
                {
                      X509_STORE_add_crl(cts, itmp->crl);
                      count++;
                }
            }
            sk_X509_INFO_pop_free(inf, X509_INFO_free);
            BIO_free(cbio);
        }
    }
    #endif/g" src/xrpld/net/detail/RegisterSSLCerts.cpp &&
    sed -i "s/#include <xrpld\/net\/RegisterSSLCerts.h>/\0\n#include <certs\/certbundle.h>/g" src/xrpld/net/detail/RegisterSSLCerts.cpp
fi
# Environment setup moved to Dockerfile in release-builder.sh
source /opt/rh/gcc-toolset-11/enable
export PATH=/usr/local/bin:$PATH
export CC='/usr/lib64/ccache/gcc' &&
export CXX='/usr/lib64/ccache/g++' &&
echo "-- Build Rippled --" &&
pwd &&

echo "MOVING TO [ build-core.sh ]";

printenv > .env.temp;
cat .env.temp | grep '=' | sed s/\\\(^[^=]\\+=\\\)/\\1\\\"/g|sed s/\$/\\\"/g > .env;
rm .env.temp;

echo "Persisting ENV:"
cat .env

./build-core.sh "$1" "$2" "$3" "$4"

echo $?
if [[ "$?" -ne "0" ]]; then
  echo "ERR build-core.sh non 0 exit code"
  exit 127
fi

echo "END [ build-core.sh ]"

echo "END INSIDE CONTAINER - FULL"

echo "-- Built with env vars:"
