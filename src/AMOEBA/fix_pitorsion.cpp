// clang-format off
/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "fix_pitorsion.h"

#include <cmath>

#include <cstring>
#include "atom.h"
#include "update.h"
#include "respa.h"
#include "domain.h"
#include "force.h"
#include "comm.h"
#include "math_const.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;
using namespace FixConst;
using namespace MathConst;

#define PITORSIONMAX 6
#define LISTDELTA 8196
#define LB_FACTOR 1.5

/* ---------------------------------------------------------------------- */

FixPiTorsion::FixPiTorsion(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg),
  pitorsion_list(nullptr), num_pitorsion(nullptr), pitorsion_type(nullptr),
  pitorsion_atom1(nullptr), pitorsion_atom2(nullptr), pitorsion_atom3(nullptr),
  pitorsion_atom4(nullptr), pitorsion_atom5(nullptr), pitorsion_atom6(nullptr)
{
  if (narg != 3) error->all(FLERR,"Illegal fix pitorsion command");

  // settings for this fix

  restart_global = 1;
  restart_peratom = 1;
  energy_global_flag = energy_peratom_flag = 1;
  virial_global_flag = virial_peratom_flag = 1;
  thermo_energy = thermo_virial = 1;
  centroidstressflag = CENTROID_NOTAVAIL;
  peratom_freq = 1;
  scalar_flag = 1;
  global_freq = 1;
  extscalar = 1;
  extvector = 1;
  wd_header = 1;
  wd_section = 1;
  respa_level_support = 1;
  ilevel_respa = 0;

  MPI_Comm_rank(world,&me);
  MPI_Comm_size(world,&nprocs);

  // perform initial allocation of atom-based arrays

  num_pitorsion = nullptr;
  pitorsion_type = nullptr;
  pitorsion_atom1 = nullptr;
  pitorsion_atom2 = nullptr;
  pitorsion_atom3 = nullptr;
  pitorsion_atom4 = nullptr;
  pitorsion_atom5 = nullptr;
  pitorsion_atom6 = nullptr;

  // register with Atom class

  nmax_previous = 0;
  grow_arrays(atom->nmax);
  atom->add_callback(Atom::GROW);
  atom->add_callback(Atom::RESTART);

  // list of all pitorsions to compute on this proc

  npitorsion_list = 0;
  max_pitorsion_list = 0;
  pitorsion_list = nullptr;
}

/* --------------------------------------------------------------------- */

FixPiTorsion::~FixPiTorsion()
{
  // unregister callbacks to this fix from Atom class

  atom->delete_callback(id,Atom::GROW);
  atom->delete_callback(id,Atom::RESTART);

  // per-atom data

  memory->destroy(num_pitorsion);
  memory->destroy(pitorsion_type);
  memory->destroy(pitorsion_atom1);
  memory->destroy(pitorsion_atom2);
  memory->destroy(pitorsion_atom3);
  memory->destroy(pitorsion_atom4);
  memory->destroy(pitorsion_atom5);
  memory->destroy(pitorsion_atom6);

  // compute list

  memory->destroy(pitorsion_list);
}

/* ---------------------------------------------------------------------- */

int FixPiTorsion::setmask()
{
  int mask = 0;
  mask |= PRE_NEIGHBOR;
  mask |= PRE_REVERSE;
  mask |= POST_FORCE;
  mask |= POST_FORCE_RESPA;
  mask |= MIN_POST_FORCE;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixPiTorsion::init()
{
  if (utils::strmatch(update->integrate_style,"^respa")) {
    ilevel_respa = ((Respa *) update->integrate)->nlevels-1;
    if (respa_level >= 0) ilevel_respa = MIN(respa_level,ilevel_respa);
  }
}

/* --------------------------------------------------------------------- */

void FixPiTorsion::setup(int vflag)
{
  pre_neighbor();

  if (utils::strmatch(update->integrate_style,"^verlet"))
    post_force(vflag);
  else {
    ((Respa *) update->integrate)->copy_flevel_f(ilevel_respa);
    post_force_respa(vflag,ilevel_respa,0);
    ((Respa *) update->integrate)->copy_f_flevel(ilevel_respa);
  }
}

/* --------------------------------------------------------------------- */

void FixPiTorsion::setup_pre_neighbor()
{
  pre_neighbor();
}

/* --------------------------------------------------------------------- */

void FixPiTorsion::setup_pre_reverse(int eflag, int vflag)
{
  pre_reverse(eflag,vflag);
}

/* --------------------------------------------------------------------- */

void FixPiTorsion::min_setup(int vflag)
{
  pre_neighbor();
  post_force(vflag);
}

/* ----------------------------------------------------------------------
   store local neighbor list for newton_bond ON
------------------------------------------------------------------------- */

void FixPiTorsion::pre_neighbor()
{
  int i,m,atom1,atom2,atom3,atom4,atom5,atom6;

  // guesstimate initial length of local pitorsion list
  // if npitorsion_global was not set (due to read_restart, no read_data),
  //   then list will grow by LISTDELTA chunks

  if (max_pitorsion_list == 0) {
    if (nprocs == 1) max_pitorsion_list = npitorsion_global;
    else max_pitorsion_list = 
           static_cast<int> (LB_FACTOR*npitorsion_global/nprocs);
    memory->create(pitorsion_list,max_pitorsion_list,7,
                   "pitorsion:pitorsion_list");
  }

  int nlocal = atom->nlocal;
  pitorsion_list = 0;

  for (i = 0; i < nlocal; i++) {
    for (m = 0; m < num_pitorsion[i]; m++) {
      atom1 = atom->map(pitorsion_atom1[i][m]);
      atom2 = atom->map(pitorsion_atom2[i][m]);
      atom3 = atom->map(pitorsion_atom3[i][m]);
      atom4 = atom->map(pitorsion_atom4[i][m]);
      atom5 = atom->map(pitorsion_atom5[i][m]);
      atom6 = atom->map(pitorsion_atom6[i][m]);

      if (atom1 == -1 || atom2 == -1 || atom3 == -1 ||
          atom4 == -1 || atom5 == -1)
        error->one(FLERR,"PiTorsion atoms {} {} {} {} {} {} missing on "
                   "proc {} at step {}",
                   pitorsion_atom1[i][m],pitorsion_atom2[i][m],
                   pitorsion_atom3[i][m],pitorsion_atom4[i][m],
                   pitorsion_atom5[i][m],pitorsion_atom6[i][m],
                   me,update->ntimestep);
      atom1 = domain->closest_image(i,atom1);
      atom2 = domain->closest_image(i,atom2);
      atom3 = domain->closest_image(i,atom3);
      atom4 = domain->closest_image(i,atom4);
      atom5 = domain->closest_image(i,atom5);
      atom6 = domain->closest_image(i,atom6);

      if (i <= atom1 && i <= atom2 && i <= atom3 &&
          i <= atom4 && i <= atom5 && i <= atom6) {
        if (npitorsion_list == max_pitorsion_list) {
          max_pitorsion_list += LISTDELTA;
          memory->grow(pitorsion_list,max_pitorsion_list,7,
                       "pitorsion:pitorsion_list");
        }
        pitorsion_list[npitorsion_list][0] = atom1;
        pitorsion_list[npitorsion_list][1] = atom2;
        pitorsion_list[npitorsion_list][2] = atom3;
        pitorsion_list[npitorsion_list][3] = atom4;
        pitorsion_list[npitorsion_list][4] = atom5;
        pitorsion_list[npitorsion_list][5] = atom6;
        pitorsion_list[npitorsion_list][6] = pitorsion_type[i][m];
        npitorsion_list++;
      }
    }
  }
}

/* ----------------------------------------------------------------------
   store eflag, so can use it in post_force to tally per-atom energies
------------------------------------------------------------------------- */

void FixPiTorsion::pre_reverse(int eflag, int /*vflag*/)
{
  eflag_caller = eflag;
}

/* ----------------------------------------------------------------------
   compute PiTorsion terms as if newton_bond = OFF, even if actually ON
------------------------------------------------------------------------- */

void FixPiTorsion::post_force(int vflag)
{
  int ia,ib,ic,id,ie,ig,ptype;
  double e,dedphi;
  double xt,yt,zt,rt2;
  double xu,yu,zu,ru2;
  double xtu,ytu,ztu;
  double rdc,rtru;
  double v2,c2,s2;
  double phi2,dphi2;
  double sine,cosine;
  double sine2,cosine2;
  double xia,yia,zia;
  double xib,yib,zib;
  double xic,yic,zic;
  double xid,yid,zid;
  double xie,yie,zie;
  double xig,yig,zig;
  double xip,yip,zip;
  double xiq,yiq,ziq;
  double xad,yad,zad;
  double xbd,ybd,zbd;
  double xec,yec,zec;
  double xgc,ygc,zgc;
  double xcp,ycp,zcp;
  double xdc,ydc,zdc;
  double xqd,yqd,zqd;
  double xdp,ydp,zdp;
  double xqc,yqc,zqc;
  double dedxt,dedyt,dedzt;
  double dedxu,dedyu,dedzu;
  double dedxia,dedyia,dedzia;
  double dedxib,dedyib,dedzib;
  double dedxic,dedyic,dedzic;
  double dedxid,dedyid,dedzid;
  double dedxie,dedyie,dedzie;
  double dedxig,dedyig,dedzig;
  double dedxip,dedyip,dedzip;
  double dedxiq,dedyiq,dedziq;
  double vxterm,vyterm,vzterm;
  double vxx,vyy,vzz;
  double vyx,vzx,vzy;

  double **x = atom->x;
  double **f = atom->f;

  epitorsion = 0.0;

  for (int n = 0; n < npitorsion_list; n++) {
    ia = pitorsion_list[n][0];
    ib = pitorsion_list[n][0];
    ic = pitorsion_list[n][0];
    id = pitorsion_list[n][0];
    ie = pitorsion_list[n][0];
    ig = pitorsion_list[n][0];
    ptype = pitorsion_list[n][6];

    // compute the value of the pi-system torsion angle

    xia = x[ia][0];
    yia = x[ia][1];
    zia = x[ia][2];
    xib = x[ib][0];
    yib = x[ib][1];
    zib = x[ib][2];
    xic = x[ic][0];
    yic = x[ic][1];
    zic = x[ic][2];
    xid = x[id][0];
    yid = x[id][1];
    zid = x[id][2];
    xie = x[ie][0];
    yie = x[ie][1];
    zie = x[ie][2];
    xig = x[ig][0];
    yig = x[ig][1];
    zig = x[ig][2];

    xad = xia - xid;
    yad = yia - yid;
    zad = zia - zid;
    xbd = xib - xid;
    ybd = yib - yid;
    zbd = zib - zid;
    xec = xie - xic;
    yec = yie - yic;
    zec = zie - zic;
    xgc = xig - xic;
    ygc = yig - yic;
    zgc = zig - zic;

    xip = yad*zbd - ybd*zad + xic;
    yip = zad*xbd - zbd*xad + yic;
    zip = xad*ybd - xbd*yad + zic;
    xiq = yec*zgc - ygc*zec + xid;
    yiq = zec*xgc - zgc*xec + yid;
    ziq = xec*ygc - xgc*yec + zid;
    xcp = xic - xip;
    ycp = yic - yip;
    zcp = zic - zip;
    xdc = xid - xic;
    ydc = yid - yic;
    zdc = zid - zic;
    xqd = xiq - xid;
    yqd = yiq - yid;
    zqd = ziq - zid;

    xt = ycp*zdc - ydc*zcp;
    yt = zcp*xdc - zdc*xcp;
    zt = xcp*ydc - xdc*ycp;
    xu = ydc*zqd - yqd*zdc;
    yu = zdc*xqd - zqd*xdc;
    zu = xdc*yqd - xqd*ydc;
    xtu = yt*zu - yu*zt;
    ytu = zt*xu - zu*xt;
    ztu = xt*yu - xu*yt;
    rt2 = xt*xt + yt*yt + zt*zt;
    ru2 = xu*xu + yu*yu + zu*zu;
    rtru = sqrt(rt2*ru2);

    if (rtru <= 0.0) continue;

    rdc = sqrt(xdc*xdc + ydc*ydc + zdc*zdc);
    cosine = (xt*xu + yt*yu + zt*zu) / rtru;
    sine = (xdc*xtu + ydc*ytu + zdc*ztu) / (rdc*rtru);

    // set the pi-system torsion parameters for this angle
        
    v2 = kpit[ptype];
    c2 = -1.0;
    s2 = 0.0;

    // compute the multiple angle trigonometry and the phase terms

    cosine2 = cosine*cosine - sine*sine;
    sine2 = 2.0 * cosine * sine;
    phi2 = 1.0 + (cosine2*c2 + sine2*s2);
    dphi2 = 2.0 * (cosine2*s2 - sine2*c2);

    // calculate pi-system torsion energy and master chain rule term

    // NOTE: is ptornunit 1.0 ?
    double ptorunit = 1.0;
    e = ptorunit * v2 * phi2;
    dedphi = ptorunit * v2 * dphi2;

    // chain rule terms for first derivative components

    xdp = xid - xip;
    ydp = yid - yip;
    zdp = zid - zip;
    xqc = xiq - xic;
    yqc = yiq - yic;
    zqc = ziq - zic;
    dedxt = dedphi * (yt*zdc - ydc*zt) / (rt2*rdc);
    dedyt = dedphi * (zt*xdc - zdc*xt) / (rt2*rdc);
    dedzt = dedphi * (xt*ydc - xdc*yt) / (rt2*rdc);
    dedxu = -dedphi * (yu*zdc - ydc*zu) / (ru2*rdc);
    dedyu = -dedphi * (zu*xdc - zdc*xu) / (ru2*rdc);
    dedzu = -dedphi * (xu*ydc - xdc*yu) / (ru2*rdc);

    // compute first derivative components for pi-system angle

    dedxip = zdc*dedyt - ydc*dedzt;
    dedyip = xdc*dedzt - zdc*dedxt;
    dedzip = ydc*dedxt - xdc*dedyt;
    dedxic = ydp*dedzt - zdp*dedyt + zqd*dedyu - yqd*dedzu;
    dedyic = zdp*dedxt - xdp*dedzt + xqd*dedzu - zqd*dedxu;
    dedzic = xdp*dedyt - ydp*dedxt + yqd*dedxu - xqd*dedyu;
    dedxid = zcp*dedyt - ycp*dedzt + yqc*dedzu - zqc*dedyu;
    dedyid = xcp*dedzt - zcp*dedxt + zqc*dedxu - xqc*dedzu;
    dedzid = ycp*dedxt - xcp*dedyt + xqc*dedyu - yqc*dedxu;
    dedxiq = zdc*dedyu - ydc*dedzu;
    dedyiq = xdc*dedzu - zdc*dedxu;
    dedziq = ydc*dedxu - xdc*dedyu;

    // compute first derivative components for individual atoms

    dedxia = ybd*dedzip - zbd*dedyip;
    dedyia = zbd*dedxip - xbd*dedzip;
    dedzia = xbd*dedyip - ybd*dedxip;
    dedxib = zad*dedyip - yad*dedzip;
    dedyib = xad*dedzip - zad*dedxip;
    dedzib = yad*dedxip - xad*dedyip;
    dedxie = ygc*dedziq - zgc*dedyiq;
    dedyie = zgc*dedxiq - xgc*dedziq;
    dedzie = xgc*dedyiq - ygc*dedxiq;
    dedxig = zec*dedyiq - yec*dedziq;
    dedyig = xec*dedziq - zec*dedxiq;
    dedzig = yec*dedxiq - xec*dedyiq;
    dedxic = dedxic + dedxip - dedxie - dedxig;
    dedyic = dedyic + dedyip - dedyie - dedyig;
    dedzic = dedzic + dedzip - dedzie - dedzig;
    dedxid = dedxid + dedxiq - dedxia - dedxib;
    dedyid = dedyid + dedyiq - dedyia - dedyib;
    dedzid = dedzid + dedziq - dedzia - dedzib;

    // increment the total pi-system torsion energy and gradient

    epitorsion += e;

    f[ia][0] += dedxia;
    f[ia][1] += dedyia;
    f[ia][2] += dedzia;
    f[ib][0] += dedxib;
    f[ib][1] += dedyib;
    f[ib][2] += dedzib;
    f[ic][0] += dedxic;
    f[ic][1] += dedyic;
    f[ic][2] += dedzic;
    f[id][0] += dedxid;
    f[id][1] += dedyid;
    f[id][2] += dedzid;
    f[ie][0] += dedxie;
    f[ie][1] += dedyie;
    f[ie][2] += dedzie;
    f[ig][0] += dedxig;
    f[ig][1] += dedyig;
    f[ig][2] += dedzig;

    // increment the internal virial tensor components

    vxterm = dedxid + dedxia + dedxib;
    vyterm = dedyid + dedyia + dedyib;
    vzterm = dedzid + dedzia + dedzib;
    vxx = xdc*vxterm + xcp*dedxip - xqd*dedxiq;
    vyx = ydc*vxterm + ycp*dedxip - yqd*dedxiq;
    vzx = zdc*vxterm + zcp*dedxip - zqd*dedxiq;
    vyy = ydc*vyterm + ycp*dedyip - yqd*dedyiq;
    vzy = zdc*vyterm + zcp*dedyip - zqd*dedyiq;
    vzz = zdc*vzterm + zcp*dedzip - zqd*dedziq;

    virial[0] += vxx;
    virial[1] += vyy;
    virial[2] += vzz;
    virial[3] += vyx;
    virial[4] += vzx;
    virial[5] += vzy;
  }
}

/* ---------------------------------------------------------------------- */

void FixPiTorsion::post_force_respa(int vflag, int ilevel, int /*iloop*/)
{
  if (ilevel == ilevel_respa) post_force(vflag);
}

/* ---------------------------------------------------------------------- */

void FixPiTorsion::min_post_force(int vflag)
{
  post_force(vflag);
}

/* ----------------------------------------------------------------------
   energy of PiTorsion term
------------------------------------------------------------------------- */

double FixPiTorsion::compute_scalar()
{
  double all;
  MPI_Allreduce(&epitorsion,&all,1,MPI_DOUBLE,MPI_SUM,world);
  return all;
}

// ----------------------------------------------------------------------
// ----------------------------------------------------------------------
// methods to read and write data file
// ----------------------------------------------------------------------
// ----------------------------------------------------------------------

void FixPiTorsion::read_data_header(char *line)
{
  if (strstr(line,"pitorsions")) {
    sscanf(line,BIGINT_FORMAT,&npitorsion_global);
  } else error->all(FLERR,"Invalid read data header line for fix pitorsion");

  // didn't set in constructor because this fix could be defined
  // before newton command

  newton_bond = force->newton_bond;
}

/* ----------------------------------------------------------------------
   unpack N lines in buf from section of data file labeled by keyword
   id_offset is applied to atomID fields if multiple data files are read
   store PiTorsion interactions as if newton_bond = OFF, even if actually ON
------------------------------------------------------------------------- */

void FixPiTorsion::read_data_section(char *keyword, int n, char *buf,
                                     tagint id_offset)
{
  int m,tmp,itype;
  tagint atom1,atom2,atom3,atom4,atom5,atom6;
  char *next;

  next = strchr(buf,'\n');
  *next = '\0';
  int nwords = utils::count_words(utils::trim_comment(buf));
  *next = '\n';

  if (nwords != 7)
    error->all(FLERR,"Incorrect {} format in data file",keyword);

  // loop over lines of PiTorsions
  // tokenize the line into values
  // add PiTorsion to one or more of my atoms, depending on newton_bond

  for (int i = 0; i < n; i++) {
    next = strchr(buf,'\n');
    *next = '\0';
    sscanf(buf,"%d %d " TAGINT_FORMAT " " TAGINT_FORMAT " " TAGINT_FORMAT
           " " TAGINT_FORMAT " " TAGINT_FORMAT " " TAGINT_FORMAT,
           &tmp,&itype,&atom1,&atom2,&atom3,&atom4,&atom5,&atom6);

    atom1 += id_offset;
    atom2 += id_offset;
    atom3 += id_offset;
    atom4 += id_offset;
    atom5 += id_offset;
    atom6 += id_offset;

    if ((m = atom->map(atom1)) >= 0) {
      if (num_pitorsion[m] == PITORSIONMAX)
        error->one(FLERR,"Too many PiTorsions for one atom");
      pitorsion_type[m][num_pitorsion[m]] = itype;
      pitorsion_atom1[m][num_pitorsion[m]] = atom1;
      pitorsion_atom2[m][num_pitorsion[m]] = atom2;
      pitorsion_atom3[m][num_pitorsion[m]] = atom3;
      pitorsion_atom4[m][num_pitorsion[m]] = atom4;
      pitorsion_atom5[m][num_pitorsion[m]] = atom5;
      pitorsion_atom6[m][num_pitorsion[m]] = atom6;
      num_pitorsion[m]++;
    }

    if ((m = atom->map(atom2)) >= 0) {
      if (num_pitorsion[m] == PITORSIONMAX)
        error->one(FLERR,"Too many PiTorsions for one atom");
      pitorsion_type[m][num_pitorsion[m]] = itype;
      pitorsion_atom1[m][num_pitorsion[m]] = atom1;
      pitorsion_atom2[m][num_pitorsion[m]] = atom2;
      pitorsion_atom3[m][num_pitorsion[m]] = atom3;
      pitorsion_atom4[m][num_pitorsion[m]] = atom4;
      pitorsion_atom5[m][num_pitorsion[m]] = atom5;
      pitorsion_atom6[m][num_pitorsion[m]] = atom6;
      num_pitorsion[m]++;
    }

    if ((m = atom->map(atom3)) >= 0) {
      if (num_pitorsion[m] == PITORSIONMAX)
        error->one(FLERR,"Too many PiTorsions for one atom");
      pitorsion_type[m][num_pitorsion[m]] = itype;
      pitorsion_atom1[m][num_pitorsion[m]] = atom1;
      pitorsion_atom2[m][num_pitorsion[m]] = atom2;
      pitorsion_atom3[m][num_pitorsion[m]] = atom3;
      pitorsion_atom4[m][num_pitorsion[m]] = atom4;
      pitorsion_atom5[m][num_pitorsion[m]] = atom5;
      pitorsion_atom6[m][num_pitorsion[m]] = atom6;
      num_pitorsion[m]++;
    }

    if ((m = atom->map(atom4)) >= 0) {
      if (num_pitorsion[m] == PITORSIONMAX)
        error->one(FLERR,"Too many PiTorsions for one atom");
      pitorsion_type[m][num_pitorsion[m]] = itype;
      pitorsion_atom1[m][num_pitorsion[m]] = atom1;
      pitorsion_atom2[m][num_pitorsion[m]] = atom2;
      pitorsion_atom3[m][num_pitorsion[m]] = atom3;
      pitorsion_atom4[m][num_pitorsion[m]] = atom4;
      pitorsion_atom5[m][num_pitorsion[m]] = atom5;
      pitorsion_atom6[m][num_pitorsion[m]] = atom6;
      num_pitorsion[m]++;
    }

    if ((m = atom->map(atom5)) >= 0) {
      if (num_pitorsion[m] == PITORSIONMAX)
        error->one(FLERR,"Too many PiTorsions for one atom");
      pitorsion_type[m][num_pitorsion[m]] = itype;
      pitorsion_atom1[m][num_pitorsion[m]] = atom1;
      pitorsion_atom2[m][num_pitorsion[m]] = atom2;
      pitorsion_atom3[m][num_pitorsion[m]] = atom3;
      pitorsion_atom4[m][num_pitorsion[m]] = atom4;
      pitorsion_atom5[m][num_pitorsion[m]] = atom5;
      pitorsion_atom6[m][num_pitorsion[m]] = atom6;
      num_pitorsion[m]++;
    }

    if ((m = atom->map(atom6)) >= 0) {
      if (num_pitorsion[m] == PITORSIONMAX)
        error->one(FLERR,"Too many PiTorsions for one atom");
      pitorsion_type[m][num_pitorsion[m]] = itype;
      pitorsion_atom1[m][num_pitorsion[m]] = atom1;
      pitorsion_atom2[m][num_pitorsion[m]] = atom2;
      pitorsion_atom3[m][num_pitorsion[m]] = atom3;
      pitorsion_atom4[m][num_pitorsion[m]] = atom4;
      pitorsion_atom5[m][num_pitorsion[m]] = atom5;
      pitorsion_atom6[m][num_pitorsion[m]] = atom6;
      num_pitorsion[m]++;
    }

    buf = next + 1;
  }
}

/* ---------------------------------------------------------------------- */

bigint FixPiTorsion::read_data_skip_lines(char * /*keyword*/)
{
  return npitorsion_global;
}

/* ----------------------------------------------------------------------
   write Mth header line to file
   only called by proc 0
------------------------------------------------------------------------- */

void FixPiTorsion::write_data_header(FILE *fp, int /*mth*/)
{
  fprintf(fp,BIGINT_FORMAT " pitorsions\n",npitorsion_global);
}

/* ----------------------------------------------------------------------
   return size I own for Mth data section
   # of data sections = 1 for this fix
   nx = # of PiTorsions owned by my local atoms
     if newton_bond off, atom only owns PiTorsion if it is atom3
   ny = columns = type + 6 atom IDs
------------------------------------------------------------------------- */

void FixPiTorsion::write_data_section_size(int /*mth*/, int &nx, int &ny)
{
  int i,m;

  tagint *tag = atom->tag;
  int nlocal = atom->nlocal;

  nx = 0;
  for (i = 0; i < nlocal; i++)
    for (m = 0; m < num_pitorsion[i]; m++)
      if (pitorsion_atom3[i][m] == tag[i]) nx++;

  // NOTE: should be a check for newton bond?

  ny = 7;
}

/* ----------------------------------------------------------------------
   pack values for Mth data section into 2d buf
   buf allocated by caller as owned PiTorsions by 7
------------------------------------------------------------------------- */

void FixPiTorsion::write_data_section_pack(int /*mth*/, double **buf)
{
  int i,m;

  // 1st column = PiTorsion type
  // 2nd-6th columns = 5 atom IDs

  tagint *tag = atom->tag;
  int nlocal = atom->nlocal;

  int n = 0;
  for (i = 0; i < nlocal; i++) {
    for (m = 0; m < num_pitorsion[i]; m++) {
      // NOTE: check for newton bond on/off ?
      if (pitorsion_atom3[i][m] != tag[i]) continue;
      buf[n][0] = ubuf(pitorsion_type[i][m]).d;
      buf[n][1] = ubuf(pitorsion_atom1[i][m]).d;
      buf[n][2] = ubuf(pitorsion_atom2[i][m]).d;
      buf[n][3] = ubuf(pitorsion_atom3[i][m]).d;
      buf[n][4] = ubuf(pitorsion_atom4[i][m]).d;
      buf[n][5] = ubuf(pitorsion_atom5[i][m]).d;
      buf[n][6] = ubuf(pitorsion_atom6[i][m]).d;
      n++;
    }
  }
}

/* ----------------------------------------------------------------------
   write section keyword for Mth data section to file
   use Molecules or Charges if that is only field, else use fix ID
   only called by proc 0
------------------------------------------------------------------------- */

void FixPiTorsion::write_data_section_keyword(int /*mth*/, FILE *fp)
{
  fprintf(fp,"\nPiTorsion\n\n");
}

/* ----------------------------------------------------------------------
   write N lines from buf to file
   convert buf fields to int or double depending on styles
   index can be used to prepend global numbering
   only called by proc 0
------------------------------------------------------------------------- */

void FixPiTorsion::write_data_section(int /*mth*/, FILE *fp,
                                  int n, double **buf, int index)
{
  for (int i = 0; i < n; i++)
    fprintf(fp,"%d %d " TAGINT_FORMAT " " TAGINT_FORMAT
            " " TAGINT_FORMAT " " TAGINT_FORMAT " " TAGINT_FORMAT "\n",
            index+i,(int) ubuf(buf[i][0]).i,(tagint) ubuf(buf[i][1]).i,
            (tagint) ubuf(buf[i][2]).i,(tagint) ubuf(buf[i][3]).i,
            (tagint) ubuf(buf[i][4]).i,(tagint) ubuf(buf[i][5]).i);
}

// ----------------------------------------------------------------------
// ----------------------------------------------------------------------
// methods for restart and communication
// ----------------------------------------------------------------------
// ----------------------------------------------------------------------

/* ----------------------------------------------------------------------
   pack entire state of Fix into one write
------------------------------------------------------------------------- */

void FixPiTorsion::write_restart(FILE *fp)
{
  if (comm->me == 0) {
    int size = sizeof(bigint);
    fwrite(&size,sizeof(int),1,fp);
    fwrite(&npitorsion_global,sizeof(bigint),1,fp);
  }
}

/* ----------------------------------------------------------------------
   use state info from restart file to restart the Fix
------------------------------------------------------------------------- */

void FixPiTorsion::restart(char *buf)
{
  npitorsion_global = *((bigint *) buf);
}

/* ----------------------------------------------------------------------
   pack values in local atom-based arrays for restart file
------------------------------------------------------------------------- */

int FixPiTorsion::pack_restart(int i, double *buf)
{
  int n = 1;
  for (int m = 0; m < num_pitorsion[i]; m++) {
    buf[n++] = ubuf(MAX(pitorsion_type[i][m],-pitorsion_type[i][m])).d;
    buf[n++] = ubuf(pitorsion_atom1[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom2[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom3[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom4[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom5[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom6[i][m]).d;
  }
  buf[0] = n;

  return n;
}

/* ----------------------------------------------------------------------
   unpack values from atom->extra array to restart the fix
------------------------------------------------------------------------- */

void FixPiTorsion::unpack_restart(int nlocal, int nth)
{
  double **extra = atom->extra;

  // skip to Nth set of extra values
  // unpack the Nth first values this way because other fixes pack them

   int n = 0;
   for (int i = 0; i < nth; i++) n += static_cast<int> (extra[nlocal][n]);

   int count = static_cast<int> (extra[nlocal][n++]);
   num_pitorsion[nlocal] = (count-1)/7;

   for (int m = 0; m < num_pitorsion[nlocal]; m++) {
     pitorsion_type[nlocal][m] = (int) ubuf(extra[nlocal][n++]).i;
     pitorsion_atom1[nlocal][m] = (tagint) ubuf(extra[nlocal][n++]).i;
     pitorsion_atom2[nlocal][m] = (tagint) ubuf(extra[nlocal][n++]).i;
     pitorsion_atom3[nlocal][m] = (tagint) ubuf(extra[nlocal][n++]).i;
     pitorsion_atom4[nlocal][m] = (tagint) ubuf(extra[nlocal][n++]).i;
     pitorsion_atom5[nlocal][m] = (tagint) ubuf(extra[nlocal][n++]).i;
     pitorsion_atom6[nlocal][m] = (tagint) ubuf(extra[nlocal][n++]).i;
   }
}

/* ----------------------------------------------------------------------
   maxsize of any atom's restart data
------------------------------------------------------------------------- */

int FixPiTorsion::maxsize_restart()
{
  return 1 + PITORSIONMAX*6;
}

/* ----------------------------------------------------------------------
   size of atom nlocal's restart data
------------------------------------------------------------------------- */

int FixPiTorsion::size_restart(int nlocal)
{
  return 1 + num_pitorsion[nlocal]*7;
}

/* ----------------------------------------------------------------------
   allocate atom-based arrays
------------------------------------------------------------------------- */

void FixPiTorsion::grow_arrays(int nmax)
{
  num_pitorsion = memory->grow(num_pitorsion,nmax,"pitorsion:num_pitorsion");
  pitorsion_type = memory->grow(pitorsion_type,nmax,PITORSIONMAX,
                                "pitorsion:pitorsion_type");
  pitorsion_atom1 = memory->grow(pitorsion_atom1,nmax,PITORSIONMAX,
                                 "pitorsion:pitorsion_atom1");
  pitorsion_atom2 = memory->grow(pitorsion_atom2,nmax,PITORSIONMAX,
                                 "pitorsion:pitorsion_atom2");
  pitorsion_atom3 = memory->grow(pitorsion_atom3,nmax,PITORSIONMAX,
                                 "pitorsion:pitorsion_atom3");
  pitorsion_atom4 = memory->grow(pitorsion_atom4,nmax,PITORSIONMAX,
                                 "pitorsion:pitorsion_atom4");
  pitorsion_atom5 = memory->grow(pitorsion_atom5,nmax,PITORSIONMAX,
                                 "pitorsion:pitorsion_atom5");
  pitorsion_atom6 = memory->grow(pitorsion_atom6,nmax,PITORSIONMAX,
                                 "pitorsion:pitorsion_atom6");

  // initialize num_pitorion to 0 for added atoms
  // may never be set for some atoms when data file is read

  for (int i = nmax_previous; i < nmax; i++) num_pitorsion[i] = 0;
  nmax_previous = nmax;
}

/* ----------------------------------------------------------------------
   copy values within local atom-based array
------------------------------------------------------------------------- */

void FixPiTorsion::copy_arrays(int i, int j, int /*delflag*/)
{
  num_pitorsion[j] = num_pitorsion[i];

  for (int k = 0; k < num_pitorsion[j]; k++) {
    pitorsion_type[j][k] = pitorsion_type[i][k];
    pitorsion_atom1[j][k] = pitorsion_atom1[i][k];
    pitorsion_atom2[j][k] = pitorsion_atom2[i][k];
    pitorsion_atom3[j][k] = pitorsion_atom3[i][k];
    pitorsion_atom4[j][k] = pitorsion_atom4[i][k];
    pitorsion_atom5[j][k] = pitorsion_atom5[i][k];
    pitorsion_atom6[j][k] = pitorsion_atom6[i][k];
  }
}

/* ----------------------------------------------------------------------
   initialize one atom's array values, called when atom is created
------------------------------------------------------------------------- */

void FixPiTorsion::set_arrays(int i)
{
  num_pitorsion[i] = 0;
}

/* ----------------------------------------------------------------------
   pack values in local atom-based array for exchange with another proc
------------------------------------------------------------------------- */

int FixPiTorsion::pack_exchange(int i, double *buf)
{
  int n = 0;
  buf[n++] = ubuf(num_pitorsion[i]).d;
  for (int m = 0; m < num_pitorsion[i]; m++) {
    buf[n++] = ubuf(pitorsion_type[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom1[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom2[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom3[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom4[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom5[i][m]).d;
    buf[n++] = ubuf(pitorsion_atom6[i][m]).d;
  }
  return n;
}

/* ----------------------------------------------------------------------
   unpack values in local atom-based array from exchange with another proc
------------------------------------------------------------------------- */

int FixPiTorsion::unpack_exchange(int nlocal, double *buf)
{
  int n = 0;
  num_pitorsion[nlocal] = (int) ubuf(buf[n++]).i;
  for (int m = 0; m < num_pitorsion[nlocal]; m++) {
    pitorsion_type[nlocal][m] = (int) ubuf(buf[n++]).i;
    pitorsion_atom1[nlocal][m] = (tagint) ubuf(buf[n++]).i;
    pitorsion_atom2[nlocal][m] = (tagint) ubuf(buf[n++]).i;
    pitorsion_atom3[nlocal][m] = (tagint) ubuf(buf[n++]).i;
    pitorsion_atom4[nlocal][m] = (tagint) ubuf(buf[n++]).i;
    pitorsion_atom5[nlocal][m] = (tagint) ubuf(buf[n++]).i;
    pitorsion_atom6[nlocal][m] = (tagint) ubuf(buf[n++]).i;
  }
  return n;
}

/* ----------------------------------------------------------------------
   memory usage of local atom-based arrays
------------------------------------------------------------------------- */

double FixPiTorsion::memory_usage()
{
  int nmax = atom->nmax;
  double bytes = (double)nmax * sizeof(int);             // num_pitorsion
  bytes += (double)nmax*PITORSIONMAX * sizeof(int);      // pitorsion_type
  bytes += (double)6*nmax*PITORSIONMAX * sizeof(int);    // pitorsion_atom 123456
  bytes += (double)7*max_pitorsion_list * sizeof(int);   // pitorsion_list
  return bytes;
}