Hawkey PackageKit Backend
----------------------------------------

This backend is designed to *replace* the yum and zif backends in PackageKit.

It uses the following libraries:

 * librepo : checking and downloading repository metadata
 * hawkey : for depsolving
 * rpm : for actually installing the packages on the system

It also uses a lot of internal glue to hold all the pieces together. These have
mostly been reused from the Zif project, hence all the Hif prefixes everywhere.

These are some key file locations:

* /var/cache/PackageKit/metadata/ : Used to store the repository metadata
* /var/cache/PackageKit/metadata/*/packages : Used for cached packages
* /etc/yum.repos.d/ : the hardcoded location for .repo files
* /etc/pki/rpm-gpg : the hardcoded location for the GPG signatures
* /etc/PackageKit/Hawkey.conf : the hardcoded PackageKit-hawkey config file
* $libdir/packagekit-backend/ : location of PackageKit backend objects

Still left to do:

* Only trusted logic
* Running the transaction
* Update metadata

Things that probably have to be fixed before this backend is useful:

* https://github.com/akozumpl/hawkey/issues/created_by/hughsie?state=open
* https://github.com/Tojaj/librepo/issues/created_by/hughsie?state=open

Things we haven't yet decided:

* yumdb or something sqlite-y?
* Log to the same file for all tools?
* How to access comps data
